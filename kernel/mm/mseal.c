#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "core/errno.h"
#include "core/klog.h"

/*
 * mseal(2) support (Linux 6.10+).
 *
 * Sealing is a per-VMA, one-way property: once a range is sealed, the core
 * MM mutation paths refuse to change its layout or protection.  The property
 * lives in vm_flags (VM_SEALED) and is enforced at the lowest common layer so
 * every caller (Linux mseal, fork COW inheritance, future Native ABI paths)
 * sees the same guarantee.  The ABI layer only translates the syscall wire
 * format and must not bypass these checks.
 *
 * Linux semantics implemented here:
 *   - mm_mseal: mark [addr, addr+len) sealed.  Ranges not fully covered by
 *     VMAs, or overlapping special regions (sysv shm), are rejected before
 *     any VMA is modified (no partial sealing on failure).
 *   - MAP_FIXED over a sealed range: refused (via mm_munmap_locked).
 *   - mprotect / munmap / mremap / madvise(DONTNEED|FREE|REMOVE) that touch
 *     a sealed VMA: refused with -EPERM.
 *   - fork COW: VM_SEALED is copied with the VMA, so children inherit seals.
 *
 * Not implemented (documented gaps): sealing of mremap-with-move into a
 * sealed target, userfaultfd interplay, and per-VMA seal counters for
 * /proc/PID/smaps "Sealed:" reporting.
 */

/* Returns 1 if any VMA overlapping [addr, addr+len) is sealed, 0 otherwise.
 * Caller must hold mm->lock. */
static int mm_range_has_sealed_locked(mm_struct_t *mm, vaddr_t addr,
                                      size_t len)
{
    vaddr_t end = addr + len;
    for (vm_area_t *v = mm->mmap; v && v->start < end; v = v->next) {
        if (v->end <= addr)
            continue;
        if (v->vm_flags & VM_SEALED)
            return 1;
        if (v->start >= end)
            break;
    }
    return 0;
}

/* Public helper used by the ABI layer to reject a full-range operation whose
 * Linux contract requires -EPERM when any covered VMA is sealed. */
int mm_mseal_range_is_sealed(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm)
        return 0;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_range_has_sealed_locked(mm, addr, len);
    spin_unlock_irqrestore(&mm->lock, flags);
    return r;
}

/* Locked variant: caller must already hold mm->lock (e.g. a madvise path
 * that keeps the lock across its whole range walk). */
int mm_mseal_range_is_sealed_locked(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm)
        return 0;
    return mm_range_has_sealed_locked(mm, addr, len);
}

int mm_mseal_locked(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm || !mm->pgdir)
        return -EINVAL;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return 0;
    len = ROUND_UP(len, PAGE_SIZE);
    vaddr_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT)
        return -ENOMEM;

    /* Validate full coverage before mutating any VMA: Linux requires the
     * whole range to be mapped, otherwise -ENOMEM and no sealing. */
    {
        vaddr_t covered = addr;
        for (vm_area_t *v = mm_find_vma(mm, covered); v && covered < end;
             v = v->next) {
            if (v->start > covered)
                return -ENOMEM;
            if (v->end > covered)
                covered = v->end;
        }
        if (covered < end)
            return -ENOMEM;
    }

    /* Refuse to seal special regions whose lifetime is kernel-owned. */
    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end;
         v = v->next) {
        if (v->start >= end || v->end <= addr)
            continue;
        if (v->vm_flags & (VM_SYSV_SHM | VM_PFNMAP | VM_VMO))
            return -EPERM;
    }

    /* Split the covered VMAs at the range edges so the seal applies to the
     * exact range and leaves neighbours fully mutable (Linux semantics). */
    if (mm_split_vma_at(mm, addr) < 0)
        return -ENOMEM;
    if (mm_split_vma_at(mm, end) < 0)
        return -ENOMEM;

    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end;
         v = v->next) {
        if (v->start >= end || v->end <= addr)
            continue;
        v->vm_flags |= VM_SEALED;
    }
    return 0;
}

int mm_mseal(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm)
        return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_mseal_locked(mm, addr, len);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}
