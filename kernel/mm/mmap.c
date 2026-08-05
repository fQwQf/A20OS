#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* Mapping creation: mmap/mmap_file/mmap_vmo plus the shared
 * file-mapping dirty sync used by writeback paths. */

#ifdef CONFIG_NOMMU
static void *nommu_alloc_aligned(size_t len, vaddr_t *addr_out)
{
    size_t alloc_len = len + PAGE_SIZE - 1;
    void *raw = kmalloc(alloc_len);
    if (!raw)
        return NULL;
    vaddr_t addr = ROUND_UP((vaddr_t)raw, PAGE_SIZE);
    memset((void *)addr, 0, len);
    if (addr_out)
        *addr_out = addr;
    return raw;
}
#endif /* CONFIG_NOMMU */

void mm_sync_shared_dirty_for_vnode(vnode_t *vn)
{
    if (!vn)
        return;

    uint64_t proc_flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || !t->mm)
            continue;
        mm_struct_t *mm = t->mm;
        spin_lock(&mm->lock);
        for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
            if (!(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_FILE))
                continue;
            if (vma->file_vnode != vn)
                continue;
            for (uint64_t va = vma->start; va < vma->end; ) {
                mm_leaf_info_t leaf;
                if (!mm_query_leaf(mm->pgdir, va, &leaf)) {
                    va += PAGE_SIZE;
                    continue;
                }
                int dirty = leaf.dirty;
                uint64_t idx = vma->file_offset + (va - vma->start);
                idx /= PAGE_SIZE;
                va = leaf.base + leaf.size;
                if (!dirty)
                    continue;

                page_cache_page_t *pcp = page_cache_get(vn, idx, 0);
                if (pcp) {
                    page_cache_mark_dirty(pcp);
                    page_cache_put(pcp);
                }
            }
        }
        spin_unlock(&mm->lock);
    }
    spin_unlock_irqrestore(&proc_lock, proc_flags);
}

vaddr_t mm_mmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                         int prot, int flags) {
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return (uint64_t)-EINVAL;
    if (len > USER_VA_LIMIT) return (uint64_t)-ENOMEM;

    pte_t ptef = mm_prot_to_pte_flags(prot);
    uint64_t vmf = VM_ANON;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL))
            return (uint64_t)-EEXIST;
        flags |= MAP_FIXED;
    }

    // 处理 MAP_FIXED 标志
    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap_locked(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

#ifdef CONFIG_NOMMU
    void *nommu_raw = NULL;
    if (addr == 0) {
        nommu_raw = nommu_alloc_aligned(len, &addr);
        if (!nommu_raw) return (uint64_t)-ENOMEM;
    }
#else
    // 查找合适的虚拟地址
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0) return (uint64_t)-ENOMEM;
    if (addr + len < addr || addr + len > USER_VA_LIMIT)
        return (uint64_t)-ENOMEM;
#endif

    // 创建新的 VMA
    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        return (uint64_t)-ENOMEM;
    }
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;
    vma->file_fd   = -1;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#endif

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
            kfree(vma);
            return (uint64_t)-ENOMEM;
        }
        vma->vm_flags |= VM_LOCKED;
        mm->locked_vm += len;
    }

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;

    return addr;
}

vaddr_t mm_mmap_file_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags, int file_fd,
                              uint64_t file_offset)
{
    if (file_fd < 0 || (file_offset & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;

    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0)
        return (uint64_t)-EINVAL;
    if (len > USER_VA_LIMIT)
        return (uint64_t)-ENOMEM;

    int rr = vfs_ref_fd(file_fd);
    if (rr < 0)
        return (uint64_t)rr;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL)) {
            vfs_close(file_fd);
            return (uint64_t)-EEXIST;
        }
        flags |= MAP_FIXED;
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap_locked(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

#ifdef CONFIG_NOMMU
    void *nommu_raw = NULL;
    if (addr == 0) {
        nommu_raw = nommu_alloc_aligned(len, &addr);
        if (!nommu_raw) {
            vfs_close(file_fd);
            return (uint64_t)-ENOMEM;
        }
    }
#else
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT) {
        vfs_close(file_fd);
        return (uint64_t)-ENOMEM;
    }
#endif

    uint64_t vmf = VM_FILE;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        vfs_close(file_fd);
        return (uint64_t)-ENOMEM;
    }
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->file_fd     = file_fd;
    vma->file_offset = file_offset;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#endif

    if ((vmf & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED)) {
        vfile_t *vf = vfs_get_file_ref(file_fd);
        if (vf && vf->vnode) {
            vnode_get(vf->vnode);
            vma->file_vnode = vf->vnode;
        }
        if (vf)
            vfs_put_file_ref(file_fd, vf);
    }

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
            if (vma->file_vnode) {
                vnode_put(vma->file_vnode);
                vma->file_vnode = NULL;
            }
            kfree(vma);
            vfs_close(file_fd);
            return (uint64_t)-ENOMEM;
        }
        vma->vm_flags |= VM_LOCKED;
        mm->locked_vm += len;
    }

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    return addr;
}

vaddr_t mm_mmap_vmo_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags, struct vmo *vmo,
                              uint64_t vmo_offset)
{
    if (!mm || !vmo)
        return (uint64_t)-EINVAL;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;

    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0)
        return (uint64_t)-EINVAL;
    if (len > USER_VA_LIMIT)
        return (uint64_t)-ENOMEM;
    if (vmo_offset >= vmo->size || len > vmo->size - vmo_offset)
        return (uint64_t)-EINVAL;
    if (vmo_offset & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL))
            return (uint64_t)-EEXIST;
        flags |= MAP_FIXED;
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap_locked(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

#ifdef CONFIG_NOMMU
    (void)vmo_offset;
    return (uint64_t)-EOPNOTSUPP;
#else
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT)
        return (uint64_t)-ENOMEM;
#endif

    uint64_t vmf = VM_VMO;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;

    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma)
        return (uint64_t)-ENOMEM;
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->file_fd     = -1;
    vma->vmo         = vmo;
    vma->vmo_offset  = vmo_offset;
    vmo_ref(vmo);

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    return addr;
}

vaddr_t mm_mmap(mm_struct_t *mm, vaddr_t addr, size_t len, int prot, int flags)
{
    if (!mm) return (uint64_t)-EINVAL;
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_locked(mm, addr, len, prot, flags);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_vma_flush_deferred(mm);
    return r;
}

vaddr_t mm_mmap_file(mm_struct_t *mm, vaddr_t addr, size_t len,
                     int prot, int flags, int file_fd, uint64_t file_offset)
{
    if (!mm) return (uint64_t)-EINVAL;
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_file_locked(mm, addr, len, prot, flags, file_fd,
                                    file_offset);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_vma_flush_deferred(mm);
    return r;
}

vaddr_t mm_mmap_vmo(mm_struct_t *mm, vaddr_t addr, size_t len,
                    int prot, int flags, struct vmo *vmo, uint64_t vmo_offset)
{
    if (!mm) return (uint64_t)-EINVAL;
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_vmo_locked(mm, addr, len, prot, flags, vmo,
                                   vmo_offset);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_vma_flush_deferred(mm);
    return r;
}
