#include "core/errno.h"
#include "core/lock.h"
#include "core/mman.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/vmo.h"
#include "fs/page_cache.h"

/*
 * VMO-backed region export and madvise/mlock helpers — ABI-agnostic MM
 * operations over the caller's own address space.
 *
 * mm_lookup_vmo_region backs the Native vm_share_region syscall: the range
 * must be fully covered by a single VM_VMO VMA and the returned VMO carries
 * its own reference.  mm_madvise_dontneed implements MADV_DONTNEED/MADV_FREE
 * page discard with the same frame-ownership classification as munmap:
 * VM_VMO and VM_PFNMAP frames stay owned by their VMO/global allocator, and
 * file-backed leaves are released through the page cache, never frame_put().
 * mm_madvise dispatches one advice to that discard, to the VMA fork-policy
 * flags, or to the answer this path gives for an advice it does not act on.
 * mm_vma_set_lock toggles the mlock-style VMA flag under mm->lock.
 */

struct vmo *mm_lookup_vmo_region(mm_struct_t *mm, vaddr_t addr, size_t len,
                                 uint32_t *prot_out)
{
    if (!mm || !len || (addr & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
        return NULL;

    uint64_t flags = spin_lock_irqsave(&mm->lock);
    mm_seg_t *vma = mm_seg_find(mm, addr);
    if (!vma || (uint64_t)vma->start > addr ||
        (uint64_t)vma->end < (uint64_t)addr + len ||
        !(vma->vm_flags & VM_VMO) || !vma->vmo) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return NULL;
    }
    struct vmo *vmo = vma->vmo;
    vmo_ref(vmo);
    uint32_t prot = mm_pte_flags_to_prot(vma->pte_flags);
    spin_unlock_irqrestore(&mm->lock, flags);
    if (prot_out)
        *prot_out = prot;
    return vmo;
}

/* Round [addr, addr+len) up to a page boundary and reject a range that wraps
 * or leaves the user window.  Both rejections matter because the walks below
 * step forward while va < end: an overflowed end is below addr, so the body
 * would run zero times and the advice would answer success having touched
 * nothing at all. */
static int mm_madvise_end(vaddr_t addr, size_t len, vaddr_t *end_out)
{
    vaddr_t end = (addr + len + PAGE_SIZE - 1) & ~(vaddr_t)(PAGE_SIZE - 1);
    if (end < addr || end > USER_VA_LIMIT) return -EINVAL;
    *end_out = end;
    return 0;
}

/* Coverage check: every page of [start, end) has to sit inside a mapping
 * record.  Step to the end of each covering record rather than one page at a
 * time: mm_seg_find() is a binary search, so a per-page probe costs
 * log2(nrecords) per page for a range that usually spans a handful of records
 * -- half a million probes for a 1GB MADV_DONTNEED where four would do. */
static int mm_madvise_covered(mm_struct_t *mm, vaddr_t start, vaddr_t end)
{
    for (vaddr_t va = start; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end) return -ENOMEM;
        va = vma->end;
    }
    return 0;
}

int mm_madvise_dontneed(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm || !len) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end;
    int r = mm_madvise_end(addr, len, &end);
    if (r < 0) return r;

    r = mm_madvise_covered(mm, addr, end);
    if (r < 0) return r;

#ifndef CONFIG_NOMMU
    mm_tlb_invalidate_begin(mm);
    uint64_t mm_flags = spin_lock_irqsave(&mm->lock);
    /* Walk record by record for the same reason as the coverage pass above:
     * hoist the VMO classification out of the per-page body and step by leaf
     * width instead of by PAGE_SIZE.  Both loops run under mm->lock, so the
     * record pointers stay valid for the span each one covers. */
    for (vaddr_t va = addr; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end) break;
        int no_frame_ref = (vma->vm_flags & (VM_PFNMAP | VM_VMO)) != 0;
        vaddr_t vma_end = vma->end;

        while (va < vma_end && va < end) {
            int level = 0;
            vaddr_t base = 0;
            size_t leaf_size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &leaf_size);
            if (!pte || !(*pte & PTE_V)) { va += PAGE_SIZE; continue; }
            /* A huge leaf that straddles either end of the request cannot be
             * dropped wholesale: pt_unmap_leaf() would discard the whole leaf,
             * including pages outside [addr, end) and outside the VMA, and the
             * cursor would then step past them.  Split it first, exactly as
             * munmap.c, mprotect.c and mremap.c do. */
            if (level > 0 &&
                (base < va || base + leaf_size > end || base + leaf_size > vma_end)) {
                int dr = mm_demote_huge_page(mm, va);
                if (dr < 0) {
                    spin_unlock_irqrestore(&mm->lock, mm_flags);
                    mm_tlb_invalidate_finish(mm);
                    return dr;
                }
                continue;
            }
            if (no_frame_ref) {
                /* VMO frames are owned by the VMO; PFNMAP leaves (vdso/vvar,
                 * framebuffer, driver BARs) carry no per-mapping frame
                 * reference at all -- vdso.c installs the single global frame
                 * with pt_map() and never frame_get()s it.  Unmapping such a
                 * PTE must drop the PTE only, or the mapping's own "release"
                 * drives a globally shared, still-executable frame to zero and
                 * buddy recycles it.  Same guard as mm/munmap.c. */
                paddr_t dummy = 0;
                if (pt_unmap_leaf(mm, va, &dummy, &base,
                                  &leaf_size, NULL) == 0) {
                    mm_tlb_note_change(mm, base, leaf_size);
                    mm_rss_sub_clamped(mm, leaf_size / PAGE_SIZE);
                    va = base + leaf_size;
                    continue;
                }
                va += PAGE_SIZE;
                continue;
            }
            paddr_t pa = 0;
            pfn_t held = phys_to_pfn(arch_pte_addr(*pte));
            /* A MAP_PRIVATE file leaf may still be the canonical page-cache
             * frame (handle_file_fault() keeps one installed read-only +
             * PTE_COW with a cache pin for the VMA's lifetime).  Its frame
             * refcount belongs to the page cache, so the frame hold/put pair
             * below would both leak the mapping's cache pin and put the
             * cache's own frame down to zero.  Same leaf test as munmap.c. */
            page_cache_page_t *held_pcp =
                mm_file_cache_mapping_get(vma, va, held);
            if (held_pcp) {
                if (mm_tlb_hold_page(mm, held_pcp) < 0) {
                    page_cache_put(held_pcp);
                    spin_unlock_irqrestore(&mm->lock, mm_flags);
                    mm_tlb_invalidate_finish(mm);
                    return -ENOMEM;
                }
            } else if (!pfn_valid(held) || mm_tlb_hold_frame(mm, held) < 0) {
                spin_unlock_irqrestore(&mm->lock, mm_flags);
                mm_tlb_invalidate_finish(mm);
                return -ENOMEM;
            }
            if (pt_unmap_leaf(mm, va, &pa, &base, &leaf_size, NULL) == 0) {
                mm_tlb_note_change(mm, base, leaf_size);
                if (pa) {
                    if (held_pcp) {
                        page_cache_put(held_pcp);
                        page_cache_put(held_pcp);
                    } else {
                        frame_put(phys_to_pfn(pa));
                    }
                    size_t pages = leaf_size / PAGE_SIZE;
                    mm_rss_sub_clamped(mm, pages);
                }
                va = base + leaf_size;
            } else {
                if (held_pcp)
                    page_cache_put(held_pcp);
                va += PAGE_SIZE;
            }
        }
        va = vma_end;
    }
    spin_unlock_irqrestore(&mm->lock, mm_flags);
    mm_tlb_invalidate_finish(mm);
#endif
    return 0;
}

/* Set or clear fork-policy bits over [start, start+len).
 *
 * The range is split at both boundaries first, so what gets marked is what
 * the caller asked for and not whichever record the range happened to fall
 * inside.  The whole range is then validated before any bit moves: the caller
 * asked for a whole range to be marked, and marking the covered part of a
 * range that turns out to have a hole would leave the rest shared with the
 * child while the advice reported failure. */
static int mm_madvise_set_fork_flags(mm_struct_t *mm, vaddr_t start, size_t len,
                                     uint64_t flags, int on)
{
    vaddr_t end;
    int r = mm_madvise_end(start, len, &end);
    if (r < 0) return r;

    uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
    for (vaddr_t va = start; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end) {
            spin_unlock_irqrestore(&mm->lock, lock_flags);
            return -ENOMEM;
        }
        /* VM_PFNMAP records (vDSO/vvar, device MMIO) are the kernel's own and
         * have no backing object a fork could clone from: mm_fork skips them
         * on the VM_DONTFORK bit and re-establishes the vDSO with its own
         * mapping call (mm/vm.c).  Clearing the bit from here would put a
         * copy of that record in the child ahead of the explicit mapping, and
         * neither insert path rejects a second record over an address range
         * that already has one.  Nothing a caller can usefully ask for
         * reaches them, so they are refused rather than honoured. */
        if (vma->vm_flags & VM_PFNMAP) {
            spin_unlock_irqrestore(&mm->lock, lock_flags);
            return -EPERM;
        }
        va = vma->end;
    }
#ifndef CONFIG_NOMMU
    r = mm_split_vma_at(mm, start);
    if (r == 0)
        r = mm_split_vma_at(mm, end);
#endif
    if (r < 0) {
        spin_unlock_irqrestore(&mm->lock, lock_flags);
        return r;
    }

    for (vaddr_t va = start; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end)
            break;
        if (on)
            vma->vm_flags |= flags;
        else
            vma->vm_flags &= ~flags;
        va = vma->end;
    }
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    return 0;
}

/*
 * Advice dispatch for the Native vm_advise entry point.  Only the advices this
 * path acts on reach a page-table or VMA mutation.  The rest are accepted and
 * ignored: several of them do have implementations under the Linux ABI
 * (HUGEPAGE/NOHUGEPAGE write VMA flags there), so "this path does not honour
 * it" is not the same as "this kernel cannot do it".  An unrecognised value
 * is -EINVAL rather than a hint, so a caller cannot mistake an unimplemented
 * advice for an applied one -- in particular a pure hint must never fall
 * through to the discard path and drop the caller's pages.
 *
 * The advices that are ignored still run the coverage check, because the Linux
 * entry point runs it for every advice before its own dispatch: the same
 * address must not answer differently depending on which ABI made the call.
 */
int mm_madvise(mm_struct_t *mm, vaddr_t addr, size_t len, int advice)
{
    if (!mm || !len) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;

    switch (advice) {
    case MADV_DONTNEED:
    case MADV_FREE:
        return mm_madvise_dontneed(mm, addr, len);
    case MADV_REMOVE:
        /* The range must survive the unmap: a later access has to bring the
         * contents back from swap.  The only swapout path this kernel has is
         * the OOM victim's own reclaim, which picks a task and walks all of
         * its swappable VMAs -- there is no per-range swapout to call, and
         * discarding instead would lose data the caller was promised to get
         * back. */
        return -ENOSYS;
    case MADV_DONTFORK:
        return mm_madvise_set_fork_flags(mm, addr, len, VM_DONTFORK, 1);
    case MADV_DOFORK:
        return mm_madvise_set_fork_flags(mm, addr, len, VM_DONTFORK, 0);
    case MADV_WIPEONFORK:
        /* WIPEONFORK implies DONTFORK: a range the child must not see the
         * contents of must not be mapped into the child at all. */
        return mm_madvise_set_fork_flags(mm, addr, len,
                                         VM_DONTFORK | VM_WIPEONFORK, 1);
    case MADV_KEEPONFORK:
        return mm_madvise_set_fork_flags(mm, addr, len, VM_WIPEONFORK, 0);
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
    case MADV_MERGEABLE:
    case MADV_UNMERGEABLE:
    case MADV_HUGEPAGE:
    case MADV_NOHUGEPAGE:
    case MADV_DONTDUMP:
    case MADV_DODUMP:
    case MADV_COLD:
    case MADV_PAGEOUT:
    case MADV_POPULATE_READ:
    case MADV_POPULATE_WRITE: {
        vaddr_t end;
        int r = mm_madvise_end(addr, len, &end);
        if (r < 0) return r;
        return mm_madvise_covered(mm, addr, end);
    }
    default:
        return -EINVAL;
    }
}

/*
 * Native ABI vm_lock reservation marker, set by mm_vma_set_lock() for the
 * ranges a20_vm_lock pins.  It is deliberately NOT VM_LOCKED: that bit is the
 * Linux mlock/oom-unevictable flag and has different consumers (munmap.c,
 * oom.c, mmap.c), so the two must not share a bit.  The canonical name belongs
 * beside the other VM_* flags in mm/vm.h; kept local here rather than widening
 * the change into that header.
 */
#define VM_NATIVE_LOCKED (1ULL << 27)

int mm_vma_set_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end, int on)
{
    if (!mm || end <= start) return -EINVAL;
    if (start & (PAGE_SIZE - 1)) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&mm->lock);
    for (vaddr_t va = start; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end) {
            spin_unlock_irqrestore(&mm->lock, flags);
            return -ENOMEM;
        }
        if (on)
            vma->vm_flags |= VM_NATIVE_LOCKED;
        else
            vma->vm_flags &= ~VM_NATIVE_LOCKED;
        va = vma->end;
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    return 0;
}
