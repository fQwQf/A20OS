#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
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

int mm_munmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;
    vaddr_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -EINVAL;

    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= end)
            break;
        if (v->end <= addr)
            continue;
        if ((v->vm_flags & VM_SYSV_SHM) &&
            (addr > v->start || end < v->end))
            return -EINVAL;
    }

    vm_area_t *vma = mm->mmap;
    while (vma) {
        vm_area_t *next = vma->next;
        if (vma->start >= end || vma->end <= addr) { vma = next; continue; }

        vaddr_t clip_start = vma->start < addr ? addr : vma->start;
        vaddr_t clip_end   = vma->end > end ? end : vma->end;

        // 释放该范围内的物理页面。遇到部分覆盖的 PMD leaf 时先降级。
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
                va += PAGE_SIZE;
                continue;
            }
            if (shared_file_vma && vma->file_vnode && (*pte & PTE_D)) {
                uint64_t idx = vma->file_offset + (va - vma->start);
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
                if (shared_file_vma && vma->file_vnode) {
                    uint64_t idx = vma->file_offset + (va - vma->start);
                    idx /= PAGE_SIZE;
                    held_pcp = page_cache_get(vma->file_vnode, idx, 0);
                    if (!held_pcp || mm_tlb_hold_page(mm, held_pcp) < 0) {
                        if (held_pcp)
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
            if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, &level) == 0) {
                mm_tlb_note_change(mm, base, size);
                if (pa) {
                    pfn_t pfn = phys_to_pfn(pa);
                    if (shared_file_vma && vma->file_vnode) {
                        /* Drop the temporary lookup and the mapping's pin.
                         * mm_tlb_hold_page() retains the final reference until
                         * every CPU has invalidated the old PTE. */
                        page_cache_put(held_pcp);
                        page_cache_put(held_pcp);
                    } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                        frame_put(pfn);
                    }
                    size_t pages = size / PAGE_SIZE;
                    mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;
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

        // 根据取消映射的范围，对 VMA 进行删除或拆分
        if (addr <= vma->start && end >= vma->end) {
            // 完全删除 VMA
            if (vma->prev) vma->prev->next = vma->next;
            else mm->mmap = vma->next;
            if (vma->next) vma->next->prev = vma->prev;
            mm_vma_defer(mm, vma);
        } else if (addr <= vma->start) {
            // 从开头部分删除
            vma->file_offset += clip_end - vma->start;
            vma->start = clip_end;
        } else if (end >= vma->end) {
            // 从结尾部分删除
            vma->end = clip_start;
        } else {
            // 从中间删除，需要拆分成两个 VMA
            vm_area_t *tail = kcalloc_atomic(1, sizeof(vm_area_t));
            if (!tail) return -ENOMEM;
            *tail = *vma;
            tail->start = clip_end;
            tail->end = vma->end;
            tail->file_offset += clip_end - vma->start;
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
        // 缩小堆，释放多余的物理页面
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
            if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, NULL) == 0) {
                mm_tlb_note_change(mm, base, size);
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = size / PAGE_SIZE;
                    mm->rss = (mm->rss > pages) ? mm->rss - pages : 0;
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
            vm_area_t *vma = kcalloc_atomic(1, sizeof(*vma));
            if (!vma)
                return mm->brk;
            vma->start = map_start;
            vma->end = map_end;
            vma->vm_flags = VM_ANON | VM_READ | VM_WRITE;
            vma->pte_flags = mm_user_brk_pte_flags();
            vma->file_fd = -1;
            mm_insert_vma(mm, vma);
            mm->total_vm += (map_end - map_start) / PAGE_SIZE;
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
