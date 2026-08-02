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

#ifdef CONFIG_NOMMU
#endif

/*
 * MM_VMA_PTE_AUDIT:
 * - VMA write paths live in mm_mmap(), mm_mmap_file(), mm_munmap(),
 *   mm_mprotect(), mm_mremap(), mm_brk(), exec loaders, fork(), and exit
 *   teardown. They must preserve sorted/non-overlapping mmap lists and account
 *   total_vm/rss/locked_vm while holding the address-space write exclusion
 *   defined by MM_LOCK_MODEL.
 * - Page-table write paths live in COW/demand faults, file/VMO faults,
 *   huge-page demotion, unmap, mprotect, brk shrink, fork COW setup, ELF exec,
 *   and user-copy dirty marking. Each path must document whether it uses a page
 *   TLB flush or full address-space flush.
 */



int mm_demote_huge_page(mm_struct_t *mm, vaddr_t addr) {
    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, addr, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || level == 0)
        return 0;
    if (size != PMD_SIZE)
        return -EOPNOTSUPP;

    paddr_t old_pa = arch_pte_addr(*pte);
    pte_t flags = arch_pte_flags(*pte);
    pfn_t old_pfn = phys_to_pfn(old_pa);
    if (!pfn_valid(old_pfn))
        return -ENOMEM;

    pfn_t pages[PMD_PAGE_COUNT];
    for (size_t i = 0; i < PMD_PAGE_COUNT; i++)
        pages[i] = PFN_NONE;

    for (size_t i = 0; i < PMD_PAGE_COUNT; i++) {
        pages[i] = pfa_alloc_page();
        if (pages[i] == PFN_NONE) {
            for (size_t j = 0; j < i; j++)
                frame_put(pages[j]);
            return -ENOMEM;
        }
        memcpy(pfn_to_virt(pages[i]),
               (void *)(old_pa + PAGE_OFFSET + i * PAGE_SIZE),
               PAGE_SIZE);
    }

    if (pt_unmap_leaf(mm->pgdir, base, NULL, NULL, NULL, NULL) < 0) {
        for (size_t i = 0; i < PMD_PAGE_COUNT; i++)
            frame_put(pages[i]);
        return -EINVAL;
    }
    frame_put(old_pfn);

    for (size_t i = 0; i < PMD_PAGE_COUNT; i++) {
        int r = pt_map(mm->pgdir, base + i * PAGE_SIZE,
                       pfn_to_phys(pages[i]), flags);
        if (r < 0)
            return r;
    }
    arch_tlb_flush();
    return 0;
}


static __attribute__((unused)) int mm_populate_shared_range(mm_struct_t *mm, vm_area_t *vma) {
    if ((vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED)) {
        vfile_t *vf = vfs_get_file_ref(vma->file_fd);
        if (!vf)
            return -EBADF;
        if (!vf->vnode) {
            vfs_put_file_ref(vma->file_fd, vf);
            return -EBADF;
        }
        uint64_t start = ROUND_DOWN(vma->start, PAGE_SIZE);
        uint64_t end = ROUND_UP(vma->end, PAGE_SIZE);
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
            if (pte && (*pte & PTE_V))
                continue;
            if (mm_shared_file_fault(mm, vma, va, vf) < 0) {
                vfs_put_file_ref(vma->file_fd, vf);
                return -ENOMEM;
            }
        }
        vfs_put_file_ref(vma->file_fd, vf);
        return 0;
    }

    uint64_t start = ROUND_DOWN(vma->start, PAGE_SIZE);
    uint64_t end = ROUND_UP(vma->end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
        if (pte && (*pte & PTE_V))
            continue;

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE)
            return -ENOMEM;
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = pt_map(mm->pgdir, va, pfn_to_phys(pfn), vma->pte_flags);
        if (r < 0) {
            frame_put(pfn);
            return r;
        }
        mm->rss++;
    }
    return 0;
}

/*
 * MAP_SHARED_DIRTY_SYNC_CONTRACT:
 * - Before writing back a vnode that may be mapped shared-writable, scan every
 *   task's page tables for VM_FILE+VM_SHARED VMAs backed by that vnode.
 * - For each present leaf with the hardware dirty bit set, mark the canonical
 *   page-cache page dirty so page_cache_writeback_* will flush it.
 * - mm->lock is held while touching VMAs and g_page_cache_lock; the lock order
 *   mm->lock -> g_page_cache_lock is already established and page_cache_get(0)
 *   does not perform blocking I/O.
 */

// 创建一个新的内存描述符
mm_struct_t *mm_create(void) {
    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm) return NULL;

    mm->pgdir = pt_create();
    if (!mm->pgdir) { kfree(mm); return NULL; }
    pt_map_kernel(mm->pgdir);  // 映射内核空间

    mm->mmap       = NULL;
    mm->brk        = 0;
    mm->start_brk  = 0;
    mm->mmap_base  = MMAP_BASE_ADDR;
    mm->stack_top  = 0;
    mm->stack_bottom = 0;
    mm->total_vm   = 0;
    mm->rss        = 0;
    mm->locked_vm  = 0;
    mm->def_flags  = 0;
    spin_init(&mm->lock);
    spin_set_debug(&mm->lock, "mm", mm);
    refcount_set(&mm->refcount, 1);
    ktrace_mm("[MMDBG] mm=%p lock=%p\n", (void *)mm, (void *)&mm->lock);
    return mm;
}

mm_struct_t *mm_get(mm_struct_t *mm) {
    if (!mm || !refcount_inc_not_zero(&mm->refcount))
        return NULL;
    return mm;
}

// 释放 VMA 对应的物理页面

// 销毁内存描述符及其所有资源
void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;
    if (!refcount_dec_and_test(&mm->refcount)) return;

    // 释放所有 VMA 及其物理页面
    vm_area_t *vma = mm->mmap;
    while (vma) {
        free_vma_pages(mm, vma);
        vm_area_t *next = vma->next;
        vma_release(vma);
        kfree(vma);
        vma = next;
    }

#ifdef CONFIG_NOMMU
    for (int i = 0; i < mm->num_nommu_allocs; i++) {
        if (mm->nommu_allocs[i]) {
            kfree(mm->nommu_allocs[i]);
            mm->nommu_allocs[i] = NULL;
        }
    }
    mm->num_nommu_allocs = 0;
#endif

    if (mm->pgdir) pt_destroy_user(mm->pgdir);
    kfree(mm);
}

// 查找包含指定地址的 VMA



// 创建内存映射（mmap 系统调用的实现）


/*
 * mm_mmap_vmo — map a VMO into an address space (Native ABI page source).
 *
 * VM_VMO is a shared mapping of the VMO's canonical frames: the VMO owns the
 * frames, demand faults materialize them on first touch, and fork shares the
 * same frames rather than copy-on-writing them.  The VMA takes one VMO
 * reference, released by vma_release() on unmap/teardown.
 */




// 取消内存映射（munmap 系统调用的实现）

// 调整堆大小（brk 系统调用的实现）

// 修改内存区域的保护属性（mprotect 系统调用的实现）

// 创建子进程的内存空间（fork 时使用，实现写时复制）
/* MM_FORK_COW_REGRESSION_GUARD:
 * 在修改父进程页表（设置 COW 标志）时持有 parent->lock，防止与父进程
 * 的并发页错误处理产生竞争。回归场景：父进程多线程或模拟并发 page
 * fault 与 mm_fork() 同时触碰同一 writable PTE，父/子必须都看到 COW
 * PTE，且旧页引用计数在最后一个 PTE 替换后才下降。
 * 原代码未加锁直接修改父进程 PTE，在以下场景会导致数据损坏：
 *   1. 多线程程序中，父进程的另一个线程同时触发页错误
 *   2. SMP 模式下，另一个 CPU 在处理父进程的页错误 */
mm_struct_t *mm_fork(mm_struct_t *parent) {
    if (!parent) return NULL;
    mm_struct_t *child = kcalloc(1, sizeof(mm_struct_t));
    if (!child) return NULL;
    pt_root_t *child_pgdir = pt_create();
    if (!child_pgdir) { kfree(child); return NULL; }
    pt_map_kernel(child_pgdir);

    vm_area_t *vma_pool = NULL;
    size_t vma_capacity = 0;
    uint64_t parent_flags;
    for (;;) {
        parent_flags = spin_lock_irqsave(&parent->lock);
        size_t needed = 0;
        for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
            if (!(pv->vm_flags & VM_DONTFORK))
                needed++;
        }
        spin_unlock_irqrestore(&parent->lock, parent_flags);

        while (vma_capacity < needed) {
            vm_area_t *node = kcalloc(1, sizeof(vm_area_t));
            if (!node) {
                while (vma_pool) {
                    node = vma_pool->next;
                    kfree(vma_pool);
                    vma_pool = node;
                }
                pt_destroy_user(child_pgdir);
                kfree(child);
                return NULL;
            }
            node->next = vma_pool;
            vma_pool = node;
            vma_capacity++;
        }

        parent_flags = spin_lock_irqsave(&parent->lock);
        needed = 0;
        for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
            if (!(pv->vm_flags & VM_DONTFORK))
                needed++;
        }
        if (needed <= vma_capacity)
            break;
        spin_unlock_irqrestore(&parent->lock, parent_flags);
    }

    *child = *parent;
    spin_init(&child->lock);
    spin_set_debug(&child->lock, "mm", child);
    refcount_set(&child->refcount, 1);
    child->rss = 0;
    child->total_vm = 0;
    child->locked_vm = 0;
    child->def_flags = 0;
    child->mmap = NULL;

    child->pgdir = child_pgdir;

    // 复制所有 VMA
    vm_area_t **tail = &child->mmap;
    vm_area_t *prev = NULL;
    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        if (pv->vm_flags & VM_DONTFORK)
            continue;
        vm_area_t *cv = vma_pool;
        vma_pool = vma_pool->next;
        vma_capacity--;
        *cv = *pv;
        cv->vm_flags &= ~VM_LOCKED;
        if (vma_ref_fork(cv) < 0) {
            vma_release_file(cv);
            kfree(cv);
            goto fail_locked;
        }
        cv->prev = prev;
        cv->next = NULL;
        *tail = cv;
        tail = &cv->next;
        prev = cv;
        child->total_vm += (cv->end - cv->start) / PAGE_SIZE;
    }

    /* parent->lock covers both the VMA snapshot and the page-table clone. */
    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        if (pv->vm_flags & (VM_DONTFORK | VM_WIPEONFORK))
            continue;
        if (pv->vm_flags & VM_VMO) {
            /* VMO mappings share canonical frames across fork: the child's
             * struct-copied VMA already holds a VMO reference (vma_ref_fork).
             * Clone already-present frames as shared and leave the rest to
             * lazy faults on the same canonical VMO pages. */
            if (mm_fork_clone_present_range(child, parent, pv->start,
                                            pv->end, 1) < 0) {
                goto fail_locked;
            }
            continue;
        }
        if (pv->vm_flags & VM_SHARED) {
            /* File-backed shared mappings are left populated on demand; fault
             * under parent->lock would perform blocking file I/O. Anonymous
             * shared mappings still need backing pages before COW logic runs. */
            if (!(pv->vm_flags & VM_FILE) &&
                mm_populate_shared_range(parent, pv) < 0) {
                goto fail_locked;
            }
        }
        if (mm_fork_clone_present_range(child, parent, pv->start, pv->end,
                                        (pv->vm_flags & VM_SHARED) != 0) < 0) {
            goto fail_locked;
        }
    }

    if (parent->start_brk < parent->brk) {
        if (mm_fork_clone_range(child, parent, parent->start_brk,
                                parent->brk, 0) < 0) {
            goto fail_locked;
        }
    }

    if (parent->stack_bottom && parent->stack_top) {
        if (mm_fork_clone_range(child, parent, parent->stack_bottom,
                                parent->stack_top, 0) < 0) {
            goto fail_locked;
        }
    }
    spin_unlock_irqrestore(&parent->lock, parent_flags);

    while (vma_pool) {
        vm_area_t *next = vma_pool->next;
        kfree(vma_pool);
        vma_pool = next;
    }

    arch_tlb_flush();
    return child;
fail_locked:
    spin_unlock_irqrestore(&parent->lock, parent_flags);
    while (vma_pool) {
        vm_area_t *next = vma_pool->next;
        kfree(vma_pool);
        vma_pool = next;
    }
    mm_destroy(child);
    return NULL;
}

#ifdef CONFIG_NOMMU
void mm_track_nommu_alloc(mm_struct_t *mm, void *ptr, size_t size, uint8_t type) {
    if (!mm || !ptr) return;
    if (mm->num_nommu_allocs < NOMMU_ALLOC_MAX) {
        mm->nommu_allocs[mm->num_nommu_allocs] = ptr;
        mm->nommu_alloc_sizes[mm->num_nommu_allocs] = size;
        mm->nommu_alloc_types[mm->num_nommu_allocs] = type;
        mm->num_nommu_allocs++;
    } else {
        printf("[NOMMU] WARNING: mm->nommu_allocs is full, leaking memory!\n");
    }
}
void mm_untrack_nommu_alloc(mm_struct_t *mm, void *ptr) {
    if (!mm || !ptr) return;
    for (int i = 0; i < mm->num_nommu_allocs; i++) {
        if (mm->nommu_allocs[i] == ptr) {
            mm->nommu_allocs[i] = mm->nommu_allocs[--mm->num_nommu_allocs];
            mm->nommu_alloc_sizes[i] = mm->nommu_alloc_sizes[mm->num_nommu_allocs];
            mm->nommu_alloc_types[i] = mm->nommu_alloc_types[mm->num_nommu_allocs];
            kfree(ptr);
            return;
        }
    }
}
#endif
