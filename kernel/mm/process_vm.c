#include "mm/process_vm.h"

#include "core/defs.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/fault.h"
#include "mm/frame.h"
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
 *
 * The target keeps running throughout, so a leaf resolved here can be unmapped
 * and buddy-freed at any moment.  Every leaf is therefore resolved under the
 * target's mm->lock and its frame is referenced for the duration of the copy:
 * the frame reference is what keeps the buddy from recycling the page out from
 * under a memcpy that has already started, and re-resolving under the lock is
 * what keeps the PTE from being freed mid-walk.
 *
 * Writes additionally have to break copy-on-write first.  A VM_COW private leaf
 * is shared with the parent's page, so writing it in place would silently
 * corrupt the original rather than the target's private copy.  handle_cow_fault()
 * performs exactly the same private-copy promotion the fault path uses, so route
 * through it instead of writing a COW leaf directly.
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

/*
 * Resolve `va` to a referenced frame in the target's address space.
 *
 * On success the frame is pinned and the out parameters describe the direct
 * mapping; the caller must frame_put() it.  Returns 0 on success, -EFAULT when
 * no usable leaf covers the address, and -EAGAIN when the leaf is a COW leaf
 * that a write must break first.
 *
 * The caller's mm->lock is taken and dropped here.  That lock is also held by
 * the fault path and by munmap, so holding it across the leaf resolution is what
 * makes the returned frame and the PTE that named it a consistent pair.  It is
 * deliberately NOT held across the caller's memcpy: that copy runs with IRQs
 * enabled and can be page-sized, and holding a spinlock across it would
 * serialise every fault and unmap in the target behind this transfer.
 */
static int process_vm_pin_leaf(mm_struct_t *mm, vaddr_t va, int for_write,
                               void **kaddr_out, size_t *avail_out,
                               pfn_t *pfn_out)
{
    *kaddr_out = NULL;
    *avail_out = 0;
    *pfn_out = PFN_NONE;

    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte) || !(*pte & PTE_U)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -EFAULT;
    }
    if (for_write && (*pte & PTE_COW)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -EAGAIN;
    }
    if (for_write && !(*pte & PTE_W)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -EFAULT;
    }
    /* A level above 0 is a huge leaf: the PTE names the block head, and `va`
     * may sit anywhere inside it.  Both the address and the length have to be
     * taken relative to that head. */
    if (va < base || va - base >= size) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -EFAULT;
    }

    pfn_t pfn = phys_to_pfn(arch_pte_addr(*pte));
    if (!pfn_valid(pfn)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -EFAULT;
    }
    /* Pin before dropping the lock: the reference is what survives a concurrent
     * unmap that frees the frame between here and the copy. */
    frame_get(pfn);
    spin_unlock_irqrestore(&mm->lock, flags);

    *pfn_out = pfn;
    *kaddr_out = (char *)pfn_to_virt(pfn) + (va - base);
    *avail_out = size - (va - base);
    return 0;
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
        pfn_t pfn;
        int r = process_vm_pin_leaf(src_task->mm, va + done, 0, &kaddr,
                                    &avail, &pfn);
        if (r < 0)
            return done > 0 ? (long)done : -EFAULT;
        if (avail > len - done)
            avail = len - done;
        memcpy((char *)dst + done, kaddr, avail);
        frame_put(pfn);
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
        pfn_t pfn;
        int r = process_vm_pin_leaf(dst_task->mm, va + done, 1, &kaddr,
                                    &avail, &pfn);
        if (r == -EAGAIN) {
            /* Promote the private copy through the fault path's own COW helper.
             * It takes mm->lock itself and does the shootdown and the deferred
             * drop of the old frame, which is why this runs with no lock held.
             * Retry the same address afterwards: the leaf is now private and
             * writable. */
            if (handle_cow_fault(dst_task, va + done) < 0)
                return done > 0 ? (long)done : -EFAULT;
            continue;
        }
        if (r < 0)
            return done > 0 ? (long)done : -EFAULT;
        if (avail > len - done)
            avail = len - done;
        memcpy(kaddr, (const char *)src + done, avail);
        frame_put(pfn);
        /* Dirty the leaf only after the data has landed, and re-resolve rather
         * than reusing the pointer: the unmap that raced the copy above may
         * have replaced it.  A missing or read-only leaf here just means the
         * write was already superseded, which is not a transfer failure. */
        (void)mm_mark_leaf_dirty_if_writable(dst_task->mm->pgdir,
                                             va + done);
        done += avail;
    }
    return (long)done;
}
