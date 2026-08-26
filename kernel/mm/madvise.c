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
    vm_area_t *vma = mm_find_vma(mm, addr);
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

    for (vaddr_t va = addr; va < end; va += PAGE_SIZE) {
        vm_area_t *vma = mm_find_vma(mm, va);
        if (!vma || va >= vma->end) return -ENOMEM;
    }

#ifndef CONFIG_NOMMU
    mm_tlb_invalidate_begin(mm);
    uint64_t mm_flags = spin_lock_irqsave(&mm->lock);
    for (vaddr_t va = addr; va < end;) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &leaf_size);
        if (!pte || !(*pte & PTE_V)) { va += PAGE_SIZE; continue; }
        vm_area_t *vma = mm_find_vma(mm, va);
        if (vma && (vma->vm_flags & VM_VMO)) {
            /* VMO frames are owned by the VMO; unmapping a PTE must not
             * frame_put() them.  Drop the PTE and let the VMO keep the
             * canonical frame. */
            paddr_t dummy = 0;
            if (pt_unmap_leaf(mm->pgdir, va, &dummy, &base,
                              &leaf_size, NULL) == 0) {
                mm_tlb_note_change(mm, base, leaf_size);
                mm->rss = (mm->rss > leaf_size / PAGE_SIZE)
                              ? mm->rss - leaf_size / PAGE_SIZE : 0;
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
        if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &leaf_size, NULL) == 0) {
            mm_tlb_note_change(mm, base, leaf_size);
            if (pa) {
                frame_put(phys_to_pfn(pa));
                size_t pages = leaf_size / PAGE_SIZE;
                mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;
            }
            va = base + leaf_size;
        } else {
            va += PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&mm->lock, mm_flags);
    mm_tlb_invalidate_finish(mm);
#endif
    return 0;
}

int mm_vma_set_lock(mm_struct_t *mm, vaddr_t start, vaddr_t end, int on)
{
    if (!mm || end <= start) return -EINVAL;
    if (start & (PAGE_SIZE - 1)) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&mm->lock);
    for (vaddr_t va = start; va < end;) {
        vm_area_t *vma = mm_find_vma(mm, va);
        if (!vma || va >= vma->end) {
            spin_unlock_irqrestore(&mm->lock, flags);
            return -ENOMEM;
        }
        if (on)
            vma->vm_flags |= 0x08000000;
        else
            vma->vm_flags &= ~(uint64_t)0x08000000;
        va = vma->end;
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    return 0;
}
