#include "mm/vm.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"

static uint64_t mm_cow_flags(uint64_t pte) {
    uint64_t flags = pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_A |
                            PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF |
                            PTE_COW);
    if (pte & (PTE_W | PTE_COW)) {
        flags &= ~(uint64_t)(PTE_W | PTE_D);
        flags |= PTE_COW;
    }
    return flags;
}

static inline size_t vm_pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

static void vma_release_file(vm_area_t *vma)
{
    if (vma && (vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
        vfs_close(vma->file_fd);
        vma->file_fd = -1;
    }
}

static int vma_ref_file(vm_area_t *vma)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || vma->file_fd < 0)
        return 0;
    return vfs_ref_fd(vma->file_fd);
}

static int vma_can_merge(vm_area_t *a, vm_area_t *b)
{
    if (!a || !b || a->end != b->start)
        return 0;
    if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_FILE) {
        if (a->file_fd != b->file_fd)
            return 0;
        return a->file_offset + (a->end - a->start) == b->file_offset;
    }
    return 1;
}

int mm_demote_huge_page(mm_struct_t *mm, uint64_t addr) {
    int level = 0;
    uint64_t base = 0;
    size_t size = 0;
    uint64_t *pte = pt_lookup_leaf(mm->pgdir, addr, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || level == 0)
        return 0;
    if (size != PMD_SIZE)
        return -EOPNOTSUPP;

    paddr_t old_pa = arch_pte_addr(*pte);
    uint64_t flags = arch_pte_flags(*pte);
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

static int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, uint64_t va,
                              int shared) {
    int level = 0;
    uint64_t base = 0;
    size_t size = 0;
    uint64_t *src = pt_lookup_leaf(parent->pgdir, va, &level, &base, &size);
    if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src) || !(*src & PTE_U))
        return 0;
    if (va != base)
        return 0;

    uint64_t *dst = pt_lookup_leaf(child->pgdir, va, NULL, NULL, NULL);
    if (dst && (*dst & PTE_V))
        return 0;

    paddr_t pa = arch_pte_addr(*src);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    uint64_t flags = shared ? arch_pte_flags(*src) : mm_cow_flags(*src);
    frame_get(pfn);

    int r = (level > 0) ? pt_map_huge(child->pgdir, base, pa, flags)
                        : pt_map(child->pgdir, base, pa, flags);
    if (r < 0) {
        frame_put(pfn);
        return r;
    }

    if (!shared && (*src & (PTE_W | PTE_COW)))
        *src = arch_pte_leaf(pa, flags);
    child->rss += size / PAGE_SIZE;
    return 0;
}

static int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                               uint64_t start, uint64_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        int r = mm_fork_clone_page(child, parent, va, shared);
        if (r < 0)
            return r;
    }
    return 0;
}

static int mm_fork_clone_leaf(mm_struct_t *child, uint64_t *src_pte,
                              uint64_t va, int level, int shared) {
    if (!src_pte || !(*src_pte & PTE_V) ||
        !arch_pte_is_leaf(*src_pte) || !(*src_pte & PTE_U))
        return 0;

    paddr_t pa = arch_pte_addr(*src_pte);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    uint64_t flags = shared ? arch_pte_flags(*src_pte) : mm_cow_flags(*src_pte);
    frame_get(pfn);

    int r = (level > 0) ? pt_map_huge(child->pgdir, va, pa, flags)
                        : pt_map(child->pgdir, va, pa, flags);
    if (r < 0) {
        frame_put(pfn);
        return r;
    }

    if (!shared && (*src_pte & (PTE_W | PTE_COW)))
        *src_pte = arch_pte_leaf(pa, flags);
    child->rss += vm_pt_level_size(level) / PAGE_SIZE;
    return 0;
}

static int mm_fork_clone_present_level(mm_struct_t *child, uint64_t *table,
                                       int level, uint64_t base,
                                       uint64_t start, uint64_t end,
                                       int shared) {
    if (!table || start >= end)
        return 0;

    size_t span = vm_pt_level_size(level);
    for (int i = 0; i < ARCH_PT_ENTRIES; i++) {
        uint64_t entry_base = base + (uint64_t)i * span;
        uint64_t entry_end = entry_base + span;
        if (entry_end <= start)
            continue;
        if (entry_base >= end)
            break;

        uint64_t *pte = &table[i];
        if (!(*pte & PTE_V))
            continue;

        if (arch_pte_is_leaf(*pte)) {
            int r = mm_fork_clone_leaf(child, pte, entry_base, level, shared);
            if (r < 0)
                return r;
            continue;
        }

        if (level > 0) {
            int r = mm_fork_clone_present_level(child, arch_pte_to_ptr(*pte),
                                                level - 1, entry_base,
                                                start, end, shared);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

static int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                       uint64_t start, uint64_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    return mm_fork_clone_present_level(child, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                                       0, start, end, shared);
}

static int mm_populate_shared_range(mm_struct_t *mm, vm_area_t *vma) {
    uint64_t start = ROUND_DOWN(vma->start, PAGE_SIZE);
    uint64_t end = ROUND_UP(vma->end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
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
    refcount_set(&mm->refcount, 1);
    return mm;
}

// 释放 VMA 对应的物理页面
static void free_vma_pages(mm_struct_t *mm, vm_area_t *vma) {
    if (!mm->pgdir) return;
    for (uint64_t va = vma->start; va < vma->end; ) {
        paddr_t pa = 0;
        uint64_t base = 0;
        size_t size = 0;
        if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, NULL) == 0) {
            if (pa) frame_put(phys_to_pfn(pa));
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
}

// 销毁内存描述符及其所有资源
void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;
    if (!refcount_dec_and_test(&mm->refcount)) return;

    // 释放所有 VMA 及其物理页面
    vm_area_t *vma = mm->mmap;
    while (vma) {
        free_vma_pages(mm, vma);
        vm_area_t *next = vma->next;
        vma_release_file(vma);
        kfree(vma);
        vma = next;
    }

    if (mm->pgdir) pt_destroy_user(mm->pgdir);
    kfree(mm);
}

// 查找包含指定地址的 VMA
vm_area_t *mm_find_vma(mm_struct_t *mm, uint64_t addr) {
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (addr < v->end && addr >= v->start) return v;
        if (v->start > addr) break;
    }
    return NULL;
}

// 在虚拟地址空间中查找一个足够大的空隙
uint64_t mm_find_gap(mm_struct_t *mm, uint64_t hint, size_t len) {
    uint64_t prev_end = hint;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

static int mm_range_overlaps(mm_struct_t *mm, uint64_t start, uint64_t len,
                             vm_area_t *ignore) {
    uint64_t end = start + len;
    if (end < start) return 1;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v == ignore) continue;
        if (v->start < end && v->end > start)
            return 1;
        if (v->start >= end) break;
    }
    return 0;
}

// 插入一个 VMA 到链表中，并尝试合并相邻的相同权限区域
void mm_insert_vma(mm_struct_t *mm, vm_area_t *newv) {
    vm_area_t **pp = &mm->mmap;
    vm_area_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;

    // 尝试与后一个 VMA 合并
    if (vma_can_merge(newv, newv->next)) {
        vm_area_t *nxt = newv->next;
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        vma_release_file(nxt);
        kfree(nxt);
    }
    // 尝试与前一个 VMA 合并
    if (vma_can_merge(newv->prev, newv)) {
        vm_area_t *prv = newv->prev;
        prv->end = newv->end;
        prv->next = newv->next;
        if (newv->next) newv->next->prev = prv;
        vma_release_file(newv);
        kfree(newv);
    }
}

int mm_split_vma_at(mm_struct_t *mm, uint64_t addr) {
    vm_area_t *v = mm_find_vma(mm, addr);
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

    vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
    if (!tail)
        return -ENOMEM;

    *tail = *v;
    tail->start = addr;
    tail->file_offset += addr - v->start;
    int fr = vma_ref_file(tail);
    if (fr < 0) {
        kfree(tail);
        return fr;
    }
    tail->prev = v;
    tail->next = v->next;
    if (tail->next)
        tail->next->prev = tail;

    v->end = addr;
    v->next = tail;
    return 0;
}

static vm_area_t *vma_split(vm_area_t *vma, uint64_t split) {
    if (!vma) return NULL;
    if (split <= vma->start || split >= vma->end) return vma;

    vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
    if (!tail) return NULL;

    *tail = *vma;
    tail->start = split;
    tail->file_offset += split - vma->start;
    if (vma_ref_file(tail) < 0) {
        kfree(tail);
        return NULL;
    }
    tail->prev = vma;
    tail->next = vma->next;
    if (tail->next) tail->next->prev = tail;

    vma->end = split;
    vma->next = tail;
    return tail;
}

static vm_area_t *vma_try_merge(vm_area_t *vma) {
    if (!vma) return NULL;

    if (vma_can_merge(vma->prev, vma)) {
        vm_area_t *prev = vma->prev;
        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) vma->next->prev = prev;
        vma_release_file(vma);
        kfree(vma);
        vma = prev;
    }

    if (vma_can_merge(vma, vma->next)) {
        vm_area_t *next = vma->next;
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) next->next->prev = vma;
        vma_release_file(next);
        kfree(next);
    }
    return vma;
}

// 将保护标志转换为页表项标志
uint64_t prot_to_pte(int prot) {
    uint64_t f = PTE_V | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF;
    if (prot & 1) f |= PTE_R;
    if (prot & 2) f |= (PTE_W | PTE_D);
    if (prot & 4) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;
    return f;
}

// 创建内存映射（mmap 系统调用的实现）
uint64_t mm_mmap(mm_struct_t *mm, uint64_t addr, size_t len, int prot, int flags) {
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return (uint64_t)-EINVAL;
    if (len > USER_VA_LIMIT) return (uint64_t)-ENOMEM;

    uint64_t ptef = prot_to_pte(prot);
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
        mm_munmap(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

    // 查找合适的虚拟地址
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0) return (uint64_t)-ENOMEM;
    if (addr + len < addr || addr + len > USER_VA_LIMIT)
        return (uint64_t)-ENOMEM;

    // 创建新的 VMA
    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return (uint64_t)-ENOMEM;
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;
    vma->file_fd   = -1;

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

uint64_t mm_mmap_file(mm_struct_t *mm, uint64_t addr, size_t len,
                      int prot, int flags, int file_fd, uint64_t file_offset)
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
        mm_munmap(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT) {
        vfs_close(file_fd);
        return (uint64_t)-ENOMEM;
    }

    uint64_t vmf = VM_FILE;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) {
        vfs_close(file_fd);
        return (uint64_t)-ENOMEM;
    }
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = prot_to_pte(prot);
    vma->file_fd     = file_fd;
    vma->file_offset = file_offset;

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
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

static int mm_clone_shared_mapping(mm_struct_t *mm, vm_area_t *src_vma,
                                   uint64_t src_addr, size_t len,
                                   int flags, uint64_t new_addr,
                                   uint64_t *out_addr) {
    if (!(src_vma->vm_flags & VM_SHARED))
        return -EINVAL;
    if (src_addr + len < src_addr || src_addr + len > src_vma->end)
        return -EINVAL;

    int prot = ((src_vma->pte_flags & PTE_R) ? PROT_READ : 0) |
               ((src_vma->pte_flags & PTE_W) ? PROT_WRITE : 0) |
               ((src_vma->pte_flags & PTE_X) ? PROT_EXEC : 0);
    uint64_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    uint64_t dst = mm_mmap(mm, target, len, prot,
                           (target ? MAP_FIXED : 0) | MAP_ANONYMOUS |
                           ((src_vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if ((int64_t)dst < 0)
        return (int)dst;

    vm_area_t *dst_vma = mm_find_vma(mm, dst);
    if (dst_vma) {
        dst_vma->vm_flags = src_vma->vm_flags;
        dst_vma->pte_flags = src_vma->pte_flags;
        dst_vma->file_fd = src_vma->file_fd;
        dst_vma->file_offset = src_vma->file_offset + (src_addr - src_vma->start);
        if (vma_ref_file(dst_vma) < 0) {
            dst_vma->file_fd = -1;
            mm_munmap(mm, dst, len);
            return -EBADF;
        }
    }

    for (uint64_t off = 0; off < len; ) {
        int level = 0;
        uint64_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = src_addr + off;
        uint64_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < src_addr || base + leaf_size > src_addr + len)) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0) {
                mm_munmap(mm, dst, len);
                return dr;
            }
            continue;
        }

        paddr_t pa = arch_pte_addr(*src);
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn)) {
            mm_munmap(mm, dst, len);
            return -ENOMEM;
        }

        frame_get(pfn);
        int r = (level > 0) ? pt_map_huge(mm->pgdir, dst + off, pa, arch_pte_flags(*src))
                            : pt_map(mm->pgdir, dst + off, pa, arch_pte_flags(*src));
        if (r < 0) {
            frame_put(pfn);
            mm_munmap(mm, dst, len);
            return r;
        }
        mm->rss += leaf_size / PAGE_SIZE;
        off += leaf_size;
    }

    *out_addr = dst;
    return 0;
}

static int mm_move_mapping_pages(mm_struct_t *mm, uint64_t old_addr,
                                 uint64_t dst, size_t len, int dontunmap) {
    for (uint64_t off = 0; off < len; ) {
        int level = 0;
        uint64_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = old_addr + off;
        uint64_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < old_addr || base + leaf_size > old_addr + len ||
                          ((dst + off) & (leaf_size - 1)))) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0)
                return dr;
            continue;
        }

        paddr_t pa = arch_pte_addr(*src);
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn))
            return -ENOMEM;

        uint64_t pte_flags = arch_pte_flags(*src);
        frame_get(pfn);
        int r = (level > 0) ? pt_map_huge(mm->pgdir, dst + off, pa, pte_flags)
                            : pt_map(mm->pgdir, dst + off, pa, pte_flags);
        if (r < 0) {
            frame_put(pfn);
            return r;
        }

        pt_unmap_leaf(mm->pgdir, src_va, NULL, NULL, NULL, NULL);
        frame_put(pfn);
        (void)dontunmap;
        off += leaf_size;
    }
    arch_tlb_flush();
    return 0;
}

int mm_mremap(mm_struct_t *mm, uint64_t old_addr, size_t old_size,
              size_t new_size, int flags, uint64_t new_addr,
              uint64_t *out_addr) {
    if (!mm || !out_addr) return -EINVAL;
    if (new_size == 0) return -EINVAL;
    if (old_addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -EINVAL;
    if ((flags & MREMAP_FIXED) &&
        (!(flags & MREMAP_MAYMOVE) || (new_addr & (PAGE_SIZE - 1))))
        return -EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL;

    size_t old_len = ROUND_UP(old_size, PAGE_SIZE);
    size_t new_len = ROUND_UP(new_size, PAGE_SIZE);
    if (new_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + new_len < old_addr)
        return -EINVAL;
    if ((flags & MREMAP_FIXED) && new_addr + new_len < new_addr)
        return -EINVAL;

    vm_area_t *vma = mm_find_vma(mm, old_addr);
    if (!vma) return -EFAULT;

    if (old_size == 0) {
        if (!(flags & MREMAP_MAYMOVE) || (flags & MREMAP_DONTUNMAP))
            return -EINVAL;
        if ((flags & MREMAP_FIXED) &&
            new_addr < old_addr + new_len && old_addr < new_addr + new_len)
            return -EINVAL;
        return mm_clone_shared_mapping(mm, vma, old_addr, new_len, flags,
                                       new_addr, out_addr);
    }

    if (old_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + old_len > vma->end)
        return -EFAULT;
    if ((flags & MREMAP_DONTUNMAP) && old_len != new_len)
        return -EINVAL;

    int r = mm_split_vma_at(mm, old_addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, old_addr + old_len);
    if (r < 0) return r;
    vma = mm_find_vma(mm, old_addr);
    if (!vma || vma->start != old_addr || vma->end < old_addr + old_len)
        return -EFAULT;

    if (new_len <= old_len && !(flags & (MREMAP_DONTUNMAP | MREMAP_FIXED))) {
        if (new_len < old_len)
            mm_munmap(mm, old_addr + new_len, old_len - new_len);
        *out_addr = old_addr;
        return 0;
    }

    if (new_len > old_len && !(flags & MREMAP_DONTUNMAP)) {
        uint64_t new_end = old_addr + new_len;
        int can_grow = !(flags & MREMAP_FIXED);
        if (old_addr + old_len != vma->end)
            can_grow = 0;
        if (can_grow && !mm_range_overlaps(mm, old_addr + old_len,
                                           new_len - old_len, vma)) {
            vma->end = new_end;
            mm->total_vm += (new_len - old_len) / PAGE_SIZE;
            vma_try_merge(vma);
            *out_addr = old_addr;
            return 0;
        }
    }

    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    if ((flags & MREMAP_FIXED) &&
        new_addr < old_addr + old_len && old_addr < new_addr + new_len)
        return -EINVAL;

    int prot = ((vma->pte_flags & PTE_R) ? PROT_READ : 0) |
               ((vma->pte_flags & PTE_W) ? PROT_WRITE : 0) |
               ((vma->pte_flags & PTE_X) ? PROT_EXEC : 0);
    uint64_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    uint64_t dst = mm_mmap(mm, target, new_len, prot,
                           (target ? MAP_FIXED : 0) | MAP_ANONYMOUS |
                           ((vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if ((int64_t)dst < 0)
        return (int)dst;

    vm_area_t *dst_vma = mm_find_vma(mm, dst);
    if (dst_vma) {
        dst_vma->vm_flags = vma->vm_flags;
        dst_vma->pte_flags = vma->pte_flags;
        dst_vma->file_fd = vma->file_fd;
        dst_vma->file_offset = vma->file_offset + (old_addr - vma->start);
        if (vma_ref_file(dst_vma) < 0) {
            dst_vma->file_fd = -1;
            mm_munmap(mm, dst, new_len);
            return -EBADF;
        }
    }

    size_t move_len = old_len < new_len ? old_len : new_len;
    r = mm_move_mapping_pages(mm, old_addr, dst, move_len,
                              (flags & MREMAP_DONTUNMAP) != 0);
    if (r < 0)
        return r;

    if (!(flags & MREMAP_DONTUNMAP))
        mm_munmap(mm, old_addr, old_len);

    *out_addr = dst;
    return 0;
}

// 取消内存映射（munmap 系统调用的实现）
int mm_munmap(mm_struct_t *mm, uint64_t addr, size_t len) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;
    uint64_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -EINVAL;

    vm_area_t *vma = mm->mmap;
    while (vma) {
        vm_area_t *next = vma->next;
        if (vma->start >= end || vma->end <= addr) { vma = next; continue; }

        uint64_t clip_start = vma->start < addr ? addr : vma->start;
        uint64_t clip_end   = vma->end > end ? end : vma->end;

        // 释放该范围内的物理页面。遇到部分覆盖的 PMD leaf 时先降级。
        for (uint64_t va = clip_start; va < clip_end; ) {
            int level = 0;
            uint64_t base = 0;
            size_t size = 0;
            uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte || !(*pte & PTE_V)) {
                va += PAGE_SIZE;
                continue;
            }
            if (level > 0 && (base < clip_start || base + size > clip_end)) {
                int dr = mm_demote_huge_page(mm, va);
                if (dr < 0) return dr;
                continue;
            }
            paddr_t pa = 0;
            if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, &level) == 0) {
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;
                }
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
        size_t freed_pages = (clip_end - clip_start) / PAGE_SIZE;
        mm->total_vm = (mm->total_vm > freed_pages) ? mm->total_vm - freed_pages : 0;
        if (vma->vm_flags & VM_LOCKED) {
            size_t locked_sz = clip_end - clip_start;
            mm->locked_vm = (mm->locked_vm >= locked_sz) ? mm->locked_vm - locked_sz : 0;
        }

        // 根据取消映射的范围，对 VMA 进行删除或拆分
        if (addr <= vma->start && end >= vma->end) {
            // 完全删除 VMA
            if (vma->prev) vma->prev->next = vma->next;
            else mm->mmap = vma->next;
            if (vma->next) vma->next->prev = vma->prev;
            vma_release_file(vma);
            kfree(vma);
        } else if (addr <= vma->start) {
            // 从开头部分删除
            vma->file_offset += clip_end - vma->start;
            vma->start = clip_end;
        } else if (end >= vma->end) {
            // 从结尾部分删除
            vma->end = clip_start;
        } else {
            // 从中间删除，需要拆分成两个 VMA
            vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
            if (!tail) return -ENOMEM;
            *tail = *vma;
            tail->start = clip_end;
            tail->end = vma->end;
            tail->file_offset += clip_end - vma->start;
            int fr = vma_ref_file(tail);
            if (fr < 0) {
                kfree(tail);
                return fr;
            }
            tail->prev = vma;
            tail->next = vma->next;
            if (vma->next) vma->next->prev = tail;
            vma->next = tail;
            vma->end = clip_start;
        }
        vma = next;
    }
    arch_tlb_flush();  // 刷新 TLB
    return 0;
}

// 调整堆大小（brk 系统调用的实现）
uint64_t mm_brk(mm_struct_t *mm, uint64_t newbrk) {
    if (!mm || !mm->pgdir) return 0;
    if (newbrk == 0) return mm->brk;
    if (newbrk < mm->start_brk || newbrk > USER_VA_LIMIT)
        return mm->brk;

    uint64_t old_brk_page = ROUND_UP(mm->brk, PAGE_SIZE);
    uint64_t new_brk_page = ROUND_UP(newbrk, PAGE_SIZE);
    if (new_brk_page < newbrk)
        return mm->brk;

    if (newbrk > mm->brk && new_brk_page > old_brk_page) {
        if (mm_range_overlaps(mm, old_brk_page,
                              new_brk_page - old_brk_page, NULL))
            return mm->brk;
    }

    if (newbrk < mm->brk) {
        // 缩小堆，释放多余的物理页面
        for (uint64_t va = new_brk_page; va < old_brk_page; ) {
            int level = 0;
            uint64_t base = 0;
            size_t size = 0;
            uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte || !(*pte & PTE_V)) {
                va += PAGE_SIZE;
                continue;
            }
            if (level > 0 && (base < new_brk_page || base + size > old_brk_page)) {
                if (mm_demote_huge_page(mm, va) < 0)
                    break;
                continue;
            }
            paddr_t pa = 0;
            if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, NULL) == 0) {
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;
                }
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
        if (new_brk_page < old_brk_page)
            arch_tlb_flush();  // 刷新 TLB
    }
    mm->brk = newbrk;
    return mm->brk;
}

// 修改内存区域的保护属性（mprotect 系统调用的实现）
int mm_mprotect(mm_struct_t *mm, uint64_t addr, size_t len, int prot) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;

    uint64_t ptef = prot_to_pte(prot);
    uint64_t vm_prot = 0;
    if (prot & 1) vm_prot |= VM_READ;
    if (prot & 2) vm_prot |= VM_WRITE;
    if (prot & 4) vm_prot |= VM_EXEC;
    uint64_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -ENOMEM;
    int touched = 0;

    uint64_t covered = addr;
    for (vm_area_t *v = mm_find_vma(mm, addr); v && covered < end; v = v->next) {
        if (v->start > covered)
            break;
        if (v->end > covered)
            covered = v->end;
    }
    if (covered < end)
        return -ENOMEM;

    int r = mm_split_vma_at(mm, addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, end);
    if (r < 0) return r;

    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end; ) {
        vm_area_t *next = v->next;
        uint64_t s = v->start < addr ? addr : v->start;
        uint64_t e = v->end > end ? end : v->end;

        if (s > v->start) {
            v = vma_split(v, s);
            if (!v) return -ENOMEM;
            next = v->next;
        }
        if (e < v->end) {
            if (!vma_split(v, e)) return -ENOMEM;
            next = v->next;
        }

        for (uint64_t va = v->start; va < v->end; ) {
            int level = 0;
            uint64_t base = 0;
            size_t size = 0;
            uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (pte && (*pte & PTE_V)) {
                if (level > 0 && (base < v->start || base + size > v->end)) {
                    int dr = mm_demote_huge_page(mm, va);
                    if (dr < 0) return dr;
                    continue;
                }
                uint64_t flags = *pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                                         PTE_G | PTE_A | PTE_D | PTE_COW |
                                         PTE_LEAF | PTE_MAT1);
                flags &= ~(uint64_t)(PTE_R | PTE_W | PTE_X | PTE_D);
                flags |= ptef & (PTE_R | PTE_W | PTE_X | PTE_D);
                if (!(ptef & PTE_W))
                    flags &= ~(uint64_t)PTE_COW;
                *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
        v->pte_flags = (v->pte_flags & ~(uint64_t)(PTE_R | PTE_W | PTE_X | PTE_D)) |
                       (ptef & (PTE_R | PTE_W | PTE_X | PTE_D));
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        v = vma_try_merge(v);
        touched = 1;
        v = v ? v->next : next;
    }

    if (touched)
        arch_tlb_flush();  // 刷新 TLB
    return 0;
}

// 创建子进程的内存空间（fork 时使用，实现写时复制）
mm_struct_t *mm_fork(mm_struct_t *parent) {
    if (!parent) return NULL;
    mm_struct_t *child = kcalloc(1, sizeof(mm_struct_t));
    if (!child) return NULL;
    *child = *parent;
    spin_init(&child->lock);
    refcount_set(&child->refcount, 1);
    child->rss = 0;
    child->total_vm = 0;
    child->locked_vm = 0;
    child->def_flags = 0;
    child->mmap = NULL;

    child->pgdir = pt_create();
    if (!child->pgdir) { kfree(child); return NULL; }
    pt_map_kernel(child->pgdir);

    // 复制所有 VMA
    vm_area_t **tail = &child->mmap;
    vm_area_t *prev = NULL;
    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        if (pv->vm_flags & VM_DONTFORK)
            continue;
        vm_area_t *cv = kcalloc(1, sizeof(vm_area_t));
        if (!cv) goto fail;
        *cv = *pv;
        cv->vm_flags &= ~VM_LOCKED;
        if (vma_ref_file(cv) < 0) {
            kfree(cv);
            goto fail;
        }
        cv->prev = prev;
        cv->next = NULL;
        *tail = cv;
        tail = &cv->next;
        prev = cv;
        child->total_vm += (cv->end - cv->start) / PAGE_SIZE;
    }

    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        if (pv->vm_flags & (VM_DONTFORK | VM_WIPEONFORK))
            continue;
        if (pv->vm_flags & VM_SHARED) {
            if (mm_populate_shared_range(parent, pv) < 0)
                goto fail;
        }
        if (mm_fork_clone_present_range(child, parent, pv->start, pv->end,
                                        (pv->vm_flags & VM_SHARED) != 0) < 0)
            goto fail;
    }

    if (parent->start_brk < parent->brk) {
        if (mm_fork_clone_range(child, parent, parent->start_brk, parent->brk, 0) < 0)
            goto fail;
    }

    if (parent->stack_bottom && parent->stack_top) {
        if (mm_fork_clone_range(child, parent, parent->stack_bottom,
                                parent->stack_top, 0) < 0)
            goto fail;
    }

    arch_tlb_flush();
    return child;
fail:
    mm_destroy(child);
    return NULL;
}
