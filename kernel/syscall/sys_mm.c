#include "syscall_internal.h"

int64_t sys_brk(uint64_t addr) {
    return (int64_t)proc_brk(addr);
}

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    int gfd = fd;
    if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
        int64_t r = syscall_get_global_fd(fd);
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

int64_t sys_madvise(uint64_t addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size, int flags, uint64_t new_addr) {
#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    if (new_size == 0) return -EINVAL;
    if (old_addr & (PAGE_SIZE - 1)) return -EINVAL;

    size_t old_len = ROUND_UP(old_size, PAGE_SIZE);
    size_t new_len = ROUND_UP(new_size, PAGE_SIZE);

    vm_area_t *vma = mm_find_vma(t->mm, old_addr);
    if (!vma || vma->start != old_addr) return -EFAULT;
    if (old_size == 0) old_len = vma->end - vma->start;
    if (!old_len) return -EINVAL;

    if (new_len <= old_len) {
        if (new_len < old_len)
            mm_munmap(t->mm, old_addr + new_len, old_len - new_len);
        return (int64_t)old_addr;
    }

    uint64_t grow_by = new_len - old_len;
    uint64_t new_end = old_addr + new_len;
    int can_grow = 1;

    if ((flags & MREMAP_FIXED) && new_addr != old_addr)
        can_grow = 0;
    for (vm_area_t *v = t->mm->mmap; v; v = v->next) {
        if (v == vma) continue;
        if (v->start < new_end && v->end > old_addr + old_len) {
            can_grow = 0;
            break;
        }
    }

    if (can_grow) {
        vma->end = new_end;
        t->mm->total_vm += grow_by / PAGE_SIZE;
        return (int64_t)old_addr;
    }

    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;

    int prot = ((vma->pte_flags & PTE_R) ? PROT_READ : 0) |
               ((vma->pte_flags & PTE_W) ? PROT_WRITE : 0) |
               ((vma->pte_flags & PTE_X) ? PROT_EXEC : 0);
    uint64_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    uint64_t dst = mm_mmap(t->mm, target, new_len, prot,
                            (target ? MAP_FIXED : 0) | MAP_ANONYMOUS | MAP_PRIVATE);
    if ((int64_t)dst < 0) return dst;

    for (uint64_t off = 0; off < old_len; off += PAGE_SIZE) {
        paddr_t pa = pt_translate(t->mm->pgdir, old_addr + off);
        if (pa == 0) continue;
        void *src = (void *)((uint64_t)pa + PAGE_OFFSET);

        void *frame = frame_alloc();
        if (!frame) {
            mm_munmap(t->mm, dst, new_len);
            return -ENOMEM;
        }
        memcpy(frame, src, PAGE_SIZE);
        paddr_t frame_pa = (paddr_t)((uint64_t)(uintptr_t)frame - PAGE_OFFSET);
        int r = pt_map(t->mm->pgdir, dst + off, frame_pa, vma->pte_flags);
        if (r < 0) {
            frame_free(frame);
            mm_munmap(t->mm, dst, new_len);
            return -ENOMEM;
        }
    }

    mm_munmap(t->mm, old_addr, old_len);
    return (int64_t)dst;
}

int64_t sys_shm_open(const char *name, int oflag, int mode) {
    (void)name; (void)oflag; (void)mode;
    return -ENOSYS;
}
