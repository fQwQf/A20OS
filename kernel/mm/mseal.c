#include "mm/vm.h"
#include "mm/pt.h"
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
 * The sealing transaction and the sealed-range query live in
 * kernel/mm/mseal_policy.rs.  What is left here is the mm_seg_t field access,
 * the split primitive, the per-PTE safe-range sidecar, and the two lock
 * wrappers.
 *
 * Not implemented (documented gaps): sealing of mremap-with-move into a
 * sealed target, userfaultfd interplay, and per-VMA seal counters for
 * /proc/PID/smaps "Sealed:" reporting.
 */

extern int mm_mseal_locked(mm_struct_t *mm, vaddr_t addr, size_t len);
extern int mm_mseal_range_is_sealed_locked(mm_struct_t *mm, vaddr_t addr,
                                           size_t len);

struct a20_mseal_rec {
    void     *vma;
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_mseal_rec) == 32, "Rust/C mseal rec ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_mseal_rec) == 24, "Rust/C mseal rec ABI");
#endif

int a20_mseal_has_pgdir(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm && mm->pgdir;
}

uintptr_t a20_mseal_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

/* The first record that ends after @va.  It skips holes on purpose: the
 * coverage check has to be able to see that a hole *is* a hole rather than
 * stop at it. */
int a20_mseal_record_from(const void *mm_ptr, uintptr_t va,
                          struct a20_mseal_rec *out)
{
    const mm_struct_t *mm = mm_ptr;
    if (!mm || !out)
        return 0;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v->end <= va)
            continue;
        out->vma = v;
        out->start = v->start;
        out->end = v->end;
        out->vm_flags = v->vm_flags;
        return 1;
    }
    return 0;
}

int a20_mseal_split_at(void *mm_ptr, uintptr_t addr)
{
    return mm_split_vma_at(mm_ptr, addr);
}

/* Mark [a, b) of one record: the one-way VM_SEALED bit, and the per-entry
 * MM_SAFE_NO_FA mark so a status-driven fault cannot install speculative
 * neighbours the seal exists to freeze. */
void a20_mseal_mark(void *mm_ptr, void *vma_ptr, uintptr_t a, uintptr_t b)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = vma_ptr;
    if (!v)
        return;
    v->vm_flags |= VM_SEALED;
    mm_pt_set_safe_range(mm, a & ~(uintptr_t)(PAGE_SIZE - 1),
                         (b + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1),
                         MM_SAFE_NO_FA, 1);
}

/* Public helper used by the ABI layer to reject a full-range operation whose
 * Linux contract requires -EPERM when any covered VMA is sealed. */
int mm_mseal_range_is_sealed(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm)
        return 0;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_mseal_range_is_sealed_locked(mm, addr, len);
    spin_unlock_irqrestore(&mm->lock, flags);
    return r;
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
