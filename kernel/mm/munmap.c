#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "mm/pt.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* Unmap paths: munmap and brk shrink. */

/* An anonymous range that mm_pt_provision_anon() reserved but that was never
 * faulted has a status mark and no PTE, so the loops below skip it and
 * pt_unmap_leaf() never sees it.  Tearing the mapping down has to retire that
 * mark: mm_fault_from_status() serves any MM_ST_ANON_VIRT it finds without
 * consulting a VMA, so a mark left behind lets a later access into the hole
 * map a page instead of failing.
 *
 * The table comes from mm_pt_leaf_table(), not from the `pte` the caller got
 * back.  On this branch `pte` is NULL whenever the address has no leaf table
 * at all, and it is also NULL when pt_lookup_leaf() merely stopped at a
 * non-present entry inside a table that DOES exist -- so deriving the table as
 * `pte - vpn` is pointer arithmetic on NULL and silently skipped the very
 * pages that were about to fault from the status.  mm_pt_leaf_table() walks
 * from the root without allocating, so the teardown path cannot conjure the
 * intermediate levels it is in the middle of destroying. */
static void mm_munmap_retire_reservation(mm_struct_t *mm, vaddr_t va)
{
    pte_t *table = mm_pt_leaf_table(mm->pgdir, va);
    if (!table || table == mm->pgdir)
        return;
    int idx = arch_pt_vpn(va, 0);
    mm_pt_node_lock(table);
    if (MM_ST_GET_CLASS(mm_pt_peek(table, 0, idx)) == MM_ST_ANON_VIRT)
        mm_pt_note_absent(table, 0, idx);
    mm_pt_node_unlock(table);
}

int mm_munmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;
    vaddr_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -EINVAL;
    mm_seg_index_invalidate(mm);

    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= end)
            break;
        if (v->end <= addr)
            continue;
        if ((v->vm_flags & VM_SYSV_SHM) &&
            (addr > v->start || end < v->end))
            return -EINVAL;
        /* mseal(2): unmap (including MAP_FIXED overwrite via mm_mmap_locked)
         * of a sealed VMA is refused before any page is released. */
        if (v->vm_flags & VM_SEALED)
            return -EPERM;
    }

    mm_seg_t *vma = mm->mmap;
    while (vma) {
        mm_seg_t *next = vma->next;
        if (vma->start >= end || vma->end <= addr) { vma = next; continue; }

        vaddr_t clip_start = vma->start < addr ? addr : vma->start;
        vaddr_t clip_end   = vma->end > end ? end : vma->end;

        /* Release the physical pages in this range.  A partially covered PMD
         * leaf is demoted to base pages first. */
        int shared_file_vma = (vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
#ifdef CONFIG_NOMMU
        (void)shared_file_vma;
        if (vma->start == clip_start && vma->end == clip_end) {
            if (vma->nommu_alloc) {
                kfree(vma->nommu_alloc);
                vma->nommu_alloc = NULL;
            } else {
                mm_untrack_nommu_alloc(mm, (void *)vma->start);
            }
        }
#else
        for (uint64_t va = clip_start; va < clip_end; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte ||
                (!(*pte & PTE_V)
#ifdef CONFIG_SWAP
                 && !pte_is_swap(*pte)
#endif
                )) {
                mm_munmap_retire_reservation(mm, va);
                va += PAGE_SIZE;
                continue;
            }
            if (shared_file_vma && vma->file_vnode && (*pte & PTE_D)) {
                uint64_t idx = vma->backing_offset + (va - vma->start);
                idx /= PAGE_SIZE;
                page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
                if (pcp) {
                    page_cache_mark_dirty(pcp);
                    page_cache_put(pcp);
                }
            }
            if (level > 0 && (base < clip_start || base + size > clip_end)) {
                int dr = mm_demote_huge_page(mm, va);
                if (dr < 0) return dr;
                continue;
            }
            page_cache_page_t *held_pcp = NULL;
            pfn_t held_pfn = PFN_NONE;
            if (*pte & PTE_V) {
                held_pfn = phys_to_pfn(arch_pte_addr(*pte));
                held_pcp = mm_file_cache_mapping_get(vma, va, held_pfn);
                if (held_pcp) {
                    if (mm_tlb_hold_page(mm, held_pcp) < 0) {
                        page_cache_put(held_pcp);
                        return -ENOMEM;
                    }
                } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                    if (!pfn_valid(held_pfn) ||
                        mm_tlb_hold_frame(mm, held_pfn) < 0)
                        return -ENOMEM;
                }
            }
            paddr_t pa = 0;
            if (pt_unmap_leaf(mm, va, &pa, &base, &size, &level) == 0) {
                mm_tlb_note_change(mm, base, size);
                if (pa) {
                    pfn_t pfn = phys_to_pfn(pa);
                    if (held_pcp) {
                        /* Drop the temporary lookup and the mapping's pin.
                         * mm_tlb_hold_page() retains the final reference until
                         * every CPU has invalidated the old PTE. */
                        page_cache_put(held_pcp);
                        page_cache_put(held_pcp);
                    } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                        frame_put(pfn);
                    }
                    size_t pages = size / PAGE_SIZE;
                    mm_rss_sub_clamped(mm, pages);
                }
                va = base + size;
            } else {
                if (held_pcp)
                    page_cache_put(held_pcp);
                va += PAGE_SIZE;
            }
        }
#endif
        size_t freed_pages = (clip_end - clip_start) / PAGE_SIZE;
        mm->total_vm = (mm->total_vm > freed_pages) ? mm->total_vm - freed_pages : 0;
        if (vma->vm_flags & VM_LOCKED) {
            size_t locked_sz = clip_end - clip_start;
            mm->locked_vm = (mm->locked_vm >= locked_sz) ? mm->locked_vm - locked_sz : 0;
        }

        if (addr <= vma->start && end >= vma->end) {
            /* Whole VMA: the range loses its backing object, so its label goes
             * with it.  The per-node collapse in pt_unmap_leaf only runs for
             * tables that held no leaves, which is exactly not the case for a
             * mapping that was never faulted.
             *
             * Only this VMA's own name is dropped.  The node entries here are
             * far coarser than the mapping and may also be naming a neighbour
             * that is still live and still correct. */
            mm_mmap_seg_retire(mm, vma, clip_start, clip_end);
            if (vma->prev) vma->prev->next = vma->next;
            else mm->mmap = vma->next;
            if (vma->next) vma->next->prev = vma->prev;
            mm_vma_defer(mm, vma);
        } else if (addr <= vma->start) {
            /* Head cut.  The surviving mapping keeps its offset adjusted, so
             * what it used to name no longer describes its new range: retire
             * the old extent and name the new one. */
            uint64_t new_off = vma->backing_offset + (clip_end - vma->start);
            vma->backing_offset = new_off;
            mm_mmap_seg_retire(mm, vma, vma->start, clip_end);
            vma->start = clip_end;
            mm_mmap_seg_annotate(mm, vma, vma->start, vma->end);
        } else if (end >= vma->end) {
            /* Tail cut.  The offset still lines up -- only the extent shrank --
             * BUT the extent is exactly what mm_pt_lookup_seg() uses to decide
             * whether this address is its own.  Leaving the old name here
             * therefore works while the cut is smaller than the mapping, and
             * silently stops working the moment the mapping shrinks below its
             * own label. */
            mm_mmap_seg_retire(mm, vma, clip_start, vma->end);
            vma->end = clip_start;
            mm_mmap_seg_annotate(mm, vma, vma->start, vma->end);
        } else {
            mm_seg_t *tail = mm_seg_new();
            if (!tail) return -ENOMEM;
            *tail = *vma;
            /* Both halves name themselves, so the old extent stops being named
             * before either half is narrowed -- otherwise the cut-away middle
             * would still resolve to this mapping, which is exactly the range
             * just freed. */
            mm_mmap_seg_retire(mm, vma, vma->start, vma->end);
            refcount_set(&tail->refcount, 1);   /* the copy brought its count */
            tail->start = clip_end;
            tail->end = vma->end;
            tail->backing_offset += clip_end - vma->start;
            int fr = vma_ref_aux(tail);
            if (fr < 0) {
                kfree(tail);
                return fr;
            }
            tail->prev = vma;
            tail->next = vma->next;
            if (vma->next) vma->next->prev = tail;
            vma->next = tail;
            vma->end = clip_start;
            /* Middle cut: two mappings now describe one old one.  BOTH halves
             * need to be named afresh, not just the tail: the head's name
             * described [old_start, old_end) and therefore no longer contains
             * [old_start, clip_start).  Lookup matches on extent, so the head's
             * own addresses stopped resolving -- which the shadow measurement
             * saw directly, as mappings that answered correctly and then
             * stopped answering after a partial unmap.
             *
             * Order matters.  The head is named FIRST, while `tail` is not yet
             * linked, so the two walks cannot both claim the node entry that
             * straddles the boundary; the tail then names itself there and the
             * head's names remain valid for their own side. */
            mm_mmap_seg_annotate(mm, vma, vma->start, vma->end);
            mm_mmap_seg_annotate(mm, tail, tail->start, tail->end);
        }
        vma = next;
    }
    return 0;
}

vaddr_t mm_brk_locked(mm_struct_t *mm, vaddr_t newbrk) {
    if (!mm || !mm->pgdir) return 0;
    if (newbrk == 0) return mm->brk;
    if (newbrk < mm->start_brk || newbrk > USER_VA_LIMIT)
        return mm->brk;

#ifdef CONFIG_NOMMU
    /* Without page tables, brk cannot grow a contiguous virtual heap. Returning
     * the unchanged break makes libc fall back to mmap-backed allocations. */
    return mm->brk;
#else
    vaddr_t old_brk_page = ROUND_UP(mm->brk, PAGE_SIZE);
    vaddr_t new_brk_page = ROUND_UP(newbrk, PAGE_SIZE);
    if (new_brk_page < newbrk)
        return mm->brk;

    if (newbrk > mm->brk) {
        vaddr_t old_brk = ROUND_UP(mm->brk, PAGE_SIZE);
        vaddr_t new_brk = ROUND_UP(newbrk, PAGE_SIZE);
        if (new_brk > old_brk) {
            if (mm_range_overlaps(mm, old_brk, new_brk - old_brk, NULL))
                return mm->brk;
        }
    }

    if (newbrk < mm->brk) {
        /* mseal(2): refuse to shrink the heap into a sealed VMA. */
        for (mm_seg_t *v = mm->mmap; v; v = v->next) {
            if (v->start >= old_brk_page)
                break;
            if (v->end <= new_brk_page)
                continue;
            if (v->vm_flags & VM_SEALED)
                return mm->brk;
        }
        for (uint64_t va = new_brk_page; va < old_brk_page; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte ||
                (!(*pte & PTE_V)
#ifdef CONFIG_SWAP
                 && !pte_is_swap(*pte)
#endif
                )) {
                mm_munmap_retire_reservation(mm, va);
                va += PAGE_SIZE;
                continue;
            }
            if (level > 0 && (base < new_brk_page || base + size > old_brk_page)) {
                if (mm_demote_huge_page(mm, va) < 0)
                    break;
                continue;
            }
            if ((*pte & PTE_V) &&
                mm_tlb_hold_frame(mm,
                                  phys_to_pfn(arch_pte_addr(*pte))) < 0)
                return mm->brk;
            paddr_t pa = 0;
            if (pt_unmap_leaf(mm, va, &pa, &base, &size, NULL) == 0) {
                mm_tlb_note_change(mm, base, size);
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    mm_rss_sub_clamped(mm, pages);
                }
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
    }
    /*
     * brk pages are installed lazily by handle_demand_fault().  They still
     * need a VMA: without it, a first write to the grown heap is rejected as
     * an unmapped address before the brk fault path can allocate a frame.
     */
    if (newbrk > mm->brk) {
        vaddr_t map_start = ROUND_UP(mm->brk, PAGE_SIZE);
        vaddr_t map_end = ROUND_UP(newbrk, PAGE_SIZE);
        if (map_end > map_start) {
            mm_seg_t *vma = mm_seg_new();
            if (!vma)
                return mm->brk;
            vma->start = map_start;
            vma->end = map_end;
            vma->vm_flags = VM_ANON | VM_READ | VM_WRITE;
            vma->pte_flags = mm_user_brk_pte_flags();
            mm_insert_vma(mm, vma);
            mm->total_vm += (map_end - map_start) / PAGE_SIZE;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
            (void)mm_pt_provision_anon(mm, map_start, map_end,
                                        mm_user_brk_pte_flags());
#endif
        }
    }
    mm->brk = newbrk;
    return mm->brk;
#endif
}

int mm_munmap(mm_struct_t *mm, vaddr_t addr, size_t len) {
    if (!mm) return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_munmap_locked(mm, addr, len);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}

vaddr_t mm_brk(mm_struct_t *mm, vaddr_t newbrk)
{
    if (!mm) return 0;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_brk_locked(mm, newbrk);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    return r;
}
