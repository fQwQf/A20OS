#include "mm/process_vm.h"

#include "core/defs.h"
#include "core/errno.h"
#include "core/string.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/proc.h"

/*
 * Cross-process memory copy.
 *
 * The current task must hold a reference to the target task (proc_find_get)
 * so its mm/pgdir cannot be torn down while we walk it.  All copies walk the
 * target page table one leaf at a time; a missing leaf (e.g. a lazily
 * populated or swapped page) stops the transfer like Linux's process_vm
 * implementation.
 */

/* Same range guard as user_range_ok() in kernel/mm/mm.c. */
static inline int process_vm_range_ok(uint64_t va, size_t n)
{
    va = (uint64_t)(vaddr_t)va;
    if (n == 0)
        return 1;
    if (va >= USER_VA_LIMIT)
        return 0;
    return n <= USER_VA_LIMIT - va;
}

long process_vm_read_kernel(struct task_t *src_task, const void *src,
                            void *dst, size_t len)
{
    if (!src_task || !src_task->mm || !src_task->mm->pgdir || !src || !dst)
        return -EINVAL;
    if (!process_vm_range_ok((uint64_t)(uintptr_t)src, len))
        return -EFAULT;

    uint64_t va = (uint64_t)(uintptr_t)src;
    size_t done = 0;
    while (done < len) {
        void *kaddr;
        size_t avail;
        if (!mm_query_leaf_kaddr(src_task->mm->pgdir, va + done, &kaddr,
                                 &avail))
            return done > 0 ? (long)done : -EFAULT;
        if (avail > len - done)
            avail = len - done;
        memcpy((char *)dst + done, kaddr, avail);
        done += avail;
    }
    return (long)done;
}

long process_vm_write_kernel(struct task_t *dst_task, void *dst,
                             const void *src, size_t len)
{
    if (!dst_task || !dst_task->mm || !dst_task->mm->pgdir || !src || !dst)
        return -EINVAL;
    if (!process_vm_range_ok((uint64_t)(uintptr_t)dst, len))
        return -EFAULT;

    uint64_t va = (uint64_t)(uintptr_t)dst;
    size_t done = 0;
    while (done < len) {
        void *kaddr;
        size_t avail;
        if (!mm_query_leaf_kaddr(dst_task->mm->pgdir, va + done, &kaddr,
                                 &avail))
            return done > 0 ? (long)done : -EFAULT;
        if (avail > len - done)
            avail = len - done;
        memcpy(kaddr, (const char *)src + done, avail);
        (void)mm_mark_leaf_dirty_if_writable(dst_task->mm->pgdir,
                                             va + done);
        done += avail;
    }
    return (long)done;
}
