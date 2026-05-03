#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "mm/fault.h"
#include "mm/frame.h"

int64_t sys_brk(uint64_t addr) {
    return (int64_t)proc_brk(addr);
}

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    int gfd = fd;
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        int64_t r = fdtable_get_current(fd);
        if (r < 0) return r;
        gfd = (int)r;
    }
    return (int64_t)proc_mmap(addr, len, prot, flags, gfd, off);
}

int64_t sys_munmap(uint64_t addr, size_t len) {
    return proc_munmap(addr, len);
}

int64_t sys_mprotect(uint64_t addr, size_t len, int prot) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    return mm_mprotect(t->mm, addr, len, prot);
}

int64_t sys_msync(uint64_t addr, size_t len, int flags) {
    const int MS_ASYNC = 1;
    const int MS_INVALIDATE = 2;
    const int MS_SYNC = 4;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) return -EINVAL;
    if ((flags & MS_ASYNC) && (flags & MS_SYNC)) return -EINVAL;
    if (len == 0) return 0;
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    uint64_t end = ROUND_UP(addr + len, PAGE_SIZE);
    if (end < addr) return -EINVAL;
    for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) return -ENOMEM;
    }
    return 0;
}

int64_t sys_madvise(uint64_t addr, size_t len, int advice) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (len == 0) return 0;
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    uint64_t start = addr;
    uint64_t end = ROUND_UP(addr + len, PAGE_SIZE);
    if (end < start) return -EINVAL;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) return -ENOMEM;
    }

    switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
        return 0;
    case MADV_DONTNEED:
    case MADV_FREE:
    case MADV_REMOVE:
    case MADV_COLD:
    case MADV_PAGEOUT:
        for (uint64_t va = start; va < end; ) {
            int level = 0;
            uint64_t base = 0;
            size_t size = 0;
            uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, &level, &base, &size);
            if (!pte || !(*pte & PTE_V)) {
                va += PAGE_SIZE;
                continue;
            }
            if (level > 0 && (base < start || base + size > end)) {
                if (mm_demote_huge_page(t->mm, va) < 0)
                    return -ENOMEM;
                continue;
            }
            paddr_t pa = 0;
            if (pt_unmap_leaf(t->mm->pgdir, va, &pa, &base, &size, NULL) == 0) {
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    t->mm->rss = (t->mm->rss > pages) ? t->mm->rss - pages : 0;
                }
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
        arch_tlb_flush();
        return 0;
    case MADV_DONTFORK:
    case MADV_DOFORK:
    case MADV_WIPEONFORK:
    case MADV_KEEPONFORK:
        for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
            if (advice == MADV_DONTFORK) vma->vm_flags |= VM_DONTFORK;
            else if (advice == MADV_DOFORK) vma->vm_flags &= ~VM_DONTFORK;
            else if (advice == MADV_WIPEONFORK) vma->vm_flags |= VM_WIPEONFORK;
            else vma->vm_flags &= ~VM_WIPEONFORK;
        }
        return 0;
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_DONTDUMP:
    case MADV_DODUMP:
        return 0;
    case MADV_HUGEPAGE: {
        if (t->policy.thp_disabled) return 0;
        for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
            vma->vm_flags |= VM_HUGEPAGE;
            vma->vm_flags &= ~VM_NOHUGEPAGE;
        }
        return 0;
    }
    case MADV_NOHUGEPAGE:
        for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
            vma->vm_flags |= VM_NOHUGEPAGE;
            vma->vm_flags &= ~VM_HUGEPAGE;
        }
        return 0;
    case MADV_POPULATE_READ:
    case MADV_POPULATE_WRITE:
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            if (handle_demand_fault(t, va) < 0) return -ENOMEM;
            if (advice == MADV_POPULATE_WRITE) {
                uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
                if (!pte || !(*pte & PTE_V) || !(*pte & PTE_W)) return -EFAULT;
            }
        }
        return 0;
    default:
        return -EINVAL;
    }
}

int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size, int flags, uint64_t new_addr) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    uint64_t out = 0;
    int r = mm_mremap(t->mm, old_addr, old_size, new_size, flags, new_addr, &out);
    return r < 0 ? r : (int64_t)out;
}

int64_t sys_shm_open(const char *name, int oflag, int mode) {
    (void)mode;
    return sys_memfd_create(name, (unsigned)(oflag & O_CLOEXEC));
}
