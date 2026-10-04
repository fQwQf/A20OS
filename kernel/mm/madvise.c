#include "core/errno.h"
#include "core/lock.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/vmo.h"

/*
 * VMO-backed region export and madvise/mlock helpers — ABI-agnostic MM
 * operations over the caller's own address space.
 *
 * mm_lookup_vmo_region backs the Native vm_share_region syscall: the range
 * must be fully covered by a single VM_VMO VMA and the returned VMO carries
 * its own reference.  mm_madvise_dontneed implements MADV_DONTNEED/MADV_FREE
 * page discard (VMO frames stay owned by their VMO).  mm_vma_set_lock
 * toggles the mlock-style VMA flag under mm->lock.
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

int mm_madvise_dontneed(mm_struct_t *mm, vaddr_t addr, size_t len)
{
    if (!mm || !len) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    vaddr_t end = (addr + len + PAGE_SIZE - 1) & ~(vaddr_t)(PAGE_SIZE - 1);

    /* Coverage check.  Step to the end of each covering mapping record rather
     * than one page at a time: mm_seg_find() is a binary search, so a per-page
     * probe costs log2(nrecords) per page for a range that usually spans a
     * handful of records -- half a million probes for a 1GB MADV_DONTNEED
     * where four would do. */
    for (vaddr_t va = addr; va < end;) {
        mm_seg_t *vma = mm_seg_find(mm, va);
        if (!vma || va >= vma->end) return -ENOMEM;
        va = vma->end;
    }

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
        int is_vmo = (vma->vm_flags & VM_VMO) != 0;
        vaddr_t vma_end = vma->end;

        while (va < vma_end && va < end) {
            int level = 0;
            vaddr_t base = 0;
            size_t leaf_size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &leaf_size);
            if (!pte || !(*pte & PTE_V)) { va += PAGE_SIZE; continue; }
            if (is_vmo) {
                /* VMO frames are owned by the VMO; unmapping a PTE must not
                 * frame_put() them.  Drop the PTE and let the VMO keep the
                 * canonical frame. */
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
            if (!pfn_valid(held) || mm_tlb_hold_frame(mm, held) < 0) {
                spin_unlock_irqrestore(&mm->lock, mm_flags);
                mm_tlb_invalidate_finish(mm);
                return -ENOMEM;
            }
            if (pt_unmap_leaf(mm, va, &pa, &base, &leaf_size, NULL) == 0) {
                mm_tlb_note_change(mm, base, leaf_size);
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = leaf_size / PAGE_SIZE;
                    mm_rss_sub_clamped(mm, pages);
                }
                va = base + leaf_size;
            } else {
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
