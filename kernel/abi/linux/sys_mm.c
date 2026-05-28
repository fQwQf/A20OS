#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "mm/fault.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/mm.h"
#include "fs/devfs.h"

static uint64_t linux_mm_lock(task_t *t)
{
    return spin_lock_irqsave(&t->mm->lock);
}

static void linux_mm_unlock(task_t *t, uint64_t flags)
{
    spin_unlock_irqrestore(&t->mm->lock, flags);
}

int64_t sys_brk(uint64_t addr) {
    return (int64_t)proc_brk(addr);
}

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) == 0 ||
        (flags & (MAP_SHARED | MAP_PRIVATE)) == (MAP_SHARED | MAP_PRIVATE)) {
        return -EINVAL;
    }
    if (len == 0) return -EINVAL;

    int gfd = fd;
    if (!(flags & MAP_ANONYMOUS)) {
        if (fd < 0) return -EBADF;
        int64_t r = fdtable_get_current(fd);
        if (r < 0) return -EBADF;
        gfd = (int)r;

        vfile_t *vf = vfs_get_file_ref(gfd);
        if (!vf) return -EBADF;

        if (devfs_is_zero_vfile(vf)) {
            flags |= MAP_ANONYMOUS;
            vfs_put_file_ref(gfd, vf);
            gfd = -1;
        } else {
            int acc = vf->flags & 3;
            if (acc == 1) { // O_WRONLY
                vfs_put_file_ref(gfd, vf);
                return -EBADF;
            }

            if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
                if (acc == 0) { // O_RDONLY
                    vfs_put_file_ref(gfd, vf);
                    return -EBADF;
                }
            }
            vfs_put_file_ref(gfd, vf);
        }
    }

    int64_t res = (int64_t)proc_mmap(addr, len, prot, flags, gfd, off);
    if (res >= 0) {
        task_t *t = proc_current();
        if (t && t->mm) {
            int populate_locked = 0;
            uint64_t mm_flags = linux_mm_lock(t);
            vm_area_t *vma = mm_find_vma(t->mm, res);
            if (vma && (vma->vm_flags & VM_LOCKED))
                populate_locked = 1;
            linux_mm_unlock(t, mm_flags);
            if (populate_locked) {
                uint64_t start = res;
                uint64_t end = ROUND_UP(start + len, PAGE_SIZE);
                for (uint64_t va = start; va < end; va += PAGE_SIZE) {
                    uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
                    if (!pte || !(*pte & PTE_V)) {
                        handle_demand_fault(t, va);
                    }
                }
            }
        }
    }
    return res;
}

int64_t sys_munmap(uint64_t addr, size_t len) {
    return proc_munmap(addr, len);
}

int64_t sys_mprotect(uint64_t addr, size_t len, int prot) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    uint64_t mm_flags = linux_mm_lock(t);
    int ret = mm_mprotect(t->mm, addr, len, prot);
    linux_mm_unlock(t, mm_flags);
    return ret;
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
    uint64_t mm_flags = linux_mm_lock(t);
    for (uint64_t va = addr; va < end; va += PAGE_SIZE) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) {
            linux_mm_unlock(t, mm_flags);
            return -ENOMEM;
        }
    }
    linux_mm_unlock(t, mm_flags);
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

    uint64_t mm_flags = linux_mm_lock(t);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) {
            linux_mm_unlock(t, mm_flags);
            return -ENOMEM;
        }
    }

    int64_t ret = 0;
    switch (advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
        break;
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
                if (mm_demote_huge_page(t->mm, va) < 0) {
                    ret = -ENOMEM;
                    goto out;
                }
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
        break;
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
        break;
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_DONTDUMP:
    case MADV_DODUMP:
        break;
    case MADV_HUGEPAGE: {
        if (t->policy.thp_disabled) break;
        for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
            vma->vm_flags |= VM_HUGEPAGE;
            vma->vm_flags &= ~VM_NOHUGEPAGE;
        }
        break;
    }
    case MADV_NOHUGEPAGE:
        for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
            vma->vm_flags |= VM_NOHUGEPAGE;
            vma->vm_flags &= ~VM_HUGEPAGE;
        }
        break;
    case MADV_POPULATE_READ:
    case MADV_POPULATE_WRITE:
        break;
    default:
        ret = -EINVAL;
        break;
    }
out:
    linux_mm_unlock(t, mm_flags);
    if (ret == 0 && (advice == MADV_POPULATE_READ || advice == MADV_POPULATE_WRITE)) {
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            if (handle_demand_fault(t, va) < 0)
                return -ENOMEM;
            if (advice == MADV_POPULATE_WRITE) {
                uint64_t flags2 = linux_mm_lock(t);
                uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
                int writable = pte && (*pte & PTE_V) && (*pte & PTE_W);
                linux_mm_unlock(t, flags2);
                if (!writable)
                    return -EFAULT;
            }
        }
    }
    return ret;
}

int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size, int flags, uint64_t new_addr) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    uint64_t out = 0;
    uint64_t mm_flags = linux_mm_lock(t);
    int r = mm_mremap(t->mm, old_addr, old_size, new_size, flags, new_addr, &out);
    linux_mm_unlock(t, mm_flags);
    return r < 0 ? r : (int64_t)out;
}

int64_t sys_shm_open(const char *name, int oflag, int mode) {
    (void)mode;
    return sys_memfd_create(name, (unsigned)(oflag & O_CLOEXEC));
}

int64_t sys_mlock(uint64_t addr, size_t len) {
    if (len == 0) return 0;
    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = ROUND_UP(addr + len, PAGE_SIZE);
    if (end < start) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    uint64_t mm_flags = linux_mm_lock(t);
    int64_t ret = 0;
    for (uint64_t va = start; va < end; ) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) {
            ret = -ENOMEM;
            goto out;
        }
        va = vma->end;
    }

    int r = mm_split_vma_at(t->mm, start);
    if (r < 0) {
        ret = r;
        goto out;
    }
    r = mm_split_vma_at(t->mm, end);
    if (r < 0) {
        ret = r;
        goto out;
    }

    size_t new_locked = 0;
    for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
        if (!(vma->vm_flags & VM_LOCKED)) {
            new_locked += (vma->end - vma->start);
        }
    }

    if (t->mm->locked_vm + new_locked > t->limits.memlock && !proc_has_cap(t, CAP_SYS_ADMIN)) {
        ret = -ENOMEM;
        goto out;
    }

    for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
        if (!(vma->vm_flags & VM_LOCKED)) {
            vma->vm_flags |= VM_LOCKED;
            t->mm->locked_vm += (vma->end - vma->start);
        }
    }

    linux_mm_unlock(t, mm_flags);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
        if (!pte || !(*pte & PTE_V)) {
            handle_demand_fault(t, va);
        }
    }
    return ret;

out:
    linux_mm_unlock(t, mm_flags);
    return ret;
}

int64_t sys_munlock(uint64_t addr, size_t len) {
    if (len == 0) return 0;
    uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = ROUND_UP(addr + len, PAGE_SIZE);
    if (end < start) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    uint64_t mm_flags = linux_mm_lock(t);
    int64_t ret = 0;
    for (uint64_t va = start; va < end; ) {
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) {
            ret = -ENOMEM;
            goto out;
        }
        va = vma->end;
    }

    int r = mm_split_vma_at(t->mm, start);
    if (r < 0) {
        ret = r;
        goto out;
    }
    r = mm_split_vma_at(t->mm, end);
    if (r < 0) {
        ret = r;
        goto out;
    }

    for (vm_area_t *vma = mm_find_vma(t->mm, start); vma && vma->start < end; vma = vma->next) {
        if (vma->vm_flags & VM_LOCKED) {
            vma->vm_flags &= ~VM_LOCKED;
            size_t vma_sz = vma->end - vma->start;
            t->mm->locked_vm = (t->mm->locked_vm >= vma_sz) ? t->mm->locked_vm - vma_sz : 0;
        }
    }

out:
    linux_mm_unlock(t, mm_flags);
    return ret;
}

int64_t sys_mlockall(int flags) {
    if (flags == 0 || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)) != 0) {
        return -EINVAL;
    }
    if ((flags & MCL_ONFAULT) && !(flags & (MCL_CURRENT | MCL_FUTURE))) {
        return -EINVAL;
    }

    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    uint64_t mm_flags = linux_mm_lock(t);
    int64_t ret = 0;
    if (flags & MCL_FUTURE) {
        t->mm->def_flags |= VM_LOCKED;
    }

    if (flags & MCL_CURRENT) {
        for (vm_area_t *vma = t->mm->mmap; vma; vma = vma->next) {
            if (!(vma->vm_flags & VM_LOCKED)) {
                size_t vma_sz = vma->end - vma->start;
                if (t->mm->locked_vm + vma_sz > t->limits.memlock && !proc_has_cap(t, CAP_SYS_ADMIN)) {
                    ret = -ENOMEM;
                    goto out;
                }
                vma->vm_flags |= VM_LOCKED;
                t->mm->locked_vm += vma_sz;
            }
            if (!(flags & MCL_ONFAULT)) {
                for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
                    (void)va;
                }
            }
        }
    }

out:
    linux_mm_unlock(t, mm_flags);
    if (ret == 0 && (flags & MCL_CURRENT) && !(flags & MCL_ONFAULT)) {
        for (vm_area_t *vma = t->mm->mmap; vma; vma = vma->next) {
            for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
                uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
                if (!pte || !(*pte & PTE_V))
                    handle_demand_fault(t, va);
            }
        }
    }
    return ret;
}

int64_t sys_munlockall(void) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    uint64_t mm_flags = linux_mm_lock(t);
    t->mm->def_flags &= ~VM_LOCKED;
    for (vm_area_t *vma = t->mm->mmap; vma; vma = vma->next) {
        vma->vm_flags &= ~VM_LOCKED;
    }
    t->mm->locked_vm = 0;
    linux_mm_unlock(t, mm_flags);
    return 0;
}

int64_t sys_mincore(uint64_t addr, size_t length, unsigned char *vec) {
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (length == 0) return 0;
    if (!vec) return -EFAULT;

    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    uint64_t start = addr;
    uint64_t end = addr + length;
    uint64_t end_aligned = ROUND_UP(end, PAGE_SIZE);
    if (end_aligned < start) return -EINVAL;

    size_t pages = (end_aligned - start) / PAGE_SIZE;

    unsigned char *snapshot = proc_scratch_buffer(pages ? pages : 1);
    if (!snapshot)
        return -ENOMEM;

    uint64_t mm_flags = linux_mm_lock(t);
    for (size_t i = 0; i < pages; i++) {
        uint64_t va = start + i * PAGE_SIZE;
        vm_area_t *vma = mm_find_vma(t->mm, va);
        if (!vma || va >= vma->end) {
            linux_mm_unlock(t, mm_flags);
            return -ENOMEM;
        }
    }

    for (size_t i = 0; i < pages; i++) {
        uint64_t va = start + i * PAGE_SIZE;
        unsigned char val = 0;
        uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, va, NULL, NULL, NULL);
        if (pte && (*pte & PTE_V)) {
            val = 1;
        }
        snapshot[i] = val;
    }

    linux_mm_unlock(t, mm_flags);
    return copy_to_user(vec, snapshot, pages) < 0 ? -EFAULT : 0;
}
