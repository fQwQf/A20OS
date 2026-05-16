#include "mm/fault.h"

#include "proc/proc.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/string.h"
#include "abi/native/vmo.h"
#include "cg/cgroup.h"

int handle_cow_fault(task_t *t, uint64_t stval) {
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t leaf_base = 0;
    size_t leaf_size = 0;
    uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, stval, NULL, &leaf_base, &leaf_size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte) || !(*pte & PTE_U))
        return -1;

    if (*pte & PTE_COW) {
        paddr_t old_pa = arch_pte_addr(*pte);
        pfn_t old_pfn = phys_to_pfn(old_pa);
        if (!pfn_valid(old_pfn)) return -1;
        int order = (leaf_size >= PMD_SIZE) ? PMD_ORDER : 0;

        pfn_t new_pfn = PFN_NONE;
        int reuse = 0;

        uint64_t pfa_flags = spin_lock_irqsave(&pfa.lock);
        uint16_t rc = pfa.meta[old_pfn].refcount;
        if (rc > 1) {
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            new_pfn = pfa_alloc(order);
            if (new_pfn == PFN_NONE) return -1;
        } else {
            reuse = 1;
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
        }

        uint64_t flags = (*pte & (PTE_R | PTE_X | PTE_U | PTE_A |
                                  PTE_G | PTE_MAT1 | PTE_LEAF)) |
                         PTE_W | PTE_D;

        if (reuse) {
            *pte = arch_pte_leaf(old_pa, flags);
        } else {
            memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), leaf_size);
            frame_put(old_pfn);
            *pte = arch_pte_leaf(pfn_to_phys(new_pfn), flags);
        }
        arch_tlb_flush_page(stval);
        return 0;
    }

    if (*pte & PTE_W) {
        uint64_t flags = (*pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                                  PTE_G | PTE_A | PTE_MAT1 |
                                  PTE_LEAF | PTE_COW)) | PTE_D;
        *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
        arch_tlb_flush_page(stval);
        return 0;
    }

    return -1;
}

/*
 * Demand paging + stack growth.
 *
 * Called for page faults after COW has been ruled out or was not applicable.
 * Handles lazy stack growth, brk pages, and anonymous VMA pages.
 */
int handle_demand_fault(task_t *t, uint64_t stval) {
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t page_va = stval & ~(PAGE_SIZE - 1);

    if (t->mm->stack_top != 0) {
        uint64_t stack_size_limit = t->limits.stack ? t->limits.stack : USER_STACK_MAX_SIZE;
        if (stack_size_limit > USER_STACK_MAX_SIZE)
            stack_size_limit = USER_STACK_MAX_SIZE;
        stack_size_limit = ROUND_UP(stack_size_limit, PAGE_SIZE);
        uint64_t stack_limit = t->mm->stack_top - stack_size_limit;
        if (page_va >= stack_limit && page_va < t->mm->stack_top) {
            uint64_t *pte = pt_walk(t->mm->pgdir, page_va, 0);
            if (pte && (*pte & PTE_V))
                return -1;

            pfn_t pfn = pfa_alloc_page();
            if (pfn == PFN_NONE) return -1;
            if (cg_mem_charge(t->cgroup, 1) != 0) {
                frame_put(pfn);
                cg_mem_oom_kill(t->cgroup);
                return -1;
            }
            memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

            uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
            int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn), pte_flags);
            if (r < 0) { cg_mem_uncharge(t->cgroup, 1); frame_put(pfn); return -1; }

            if (page_va < t->mm->stack_bottom)
                t->mm->stack_bottom = page_va;
            t->mm->rss++;
            arch_tlb_flush_page(stval);
            return 0;
        }
    }

    if (page_va >= t->mm->start_brk &&
        page_va < ROUND_UP(t->mm->brk, PAGE_SIZE)) {
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) { cg_mem_uncharge(t->cgroup, 1); return -1; }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        uint64_t pte_flags = PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D | PTE_MAT1 | PTE_LEAF;
        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn), pte_flags);
        if (r < 0) { cg_mem_uncharge(t->cgroup, 1); frame_put(pfn); return -1; }

        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }

    vm_area_t *vma = mm_find_vma(t->mm, page_va);
    if (vma) {
        uint64_t *pte = pt_lookup_leaf(t->mm->pgdir, page_va, NULL, NULL, NULL);
        if (pte && (*pte & PTE_V)) return -1;
        if (!(vma->pte_flags & (PTE_R | PTE_W | PTE_X))) return -1;

        if ((vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
            vfile_t *vf = vfs_get_file_ref(vma->file_fd);
            if (!vf)
                return -1;
            if (!vf->vnode) {
                vfs_put_file_ref(vma->file_fd, vf);
                return -1;
            }

            uint64_t file_pos = vma->file_offset + (page_va - vma->start);
            page_cache_page_t *pcp = page_cache_get(vf->vnode,
                                                     file_pos / PAGE_SIZE, 1);
            if (!pcp) {
                vfs_put_file_ref(vma->file_fd, vf);
                return -1;
            }
            if (!page_cache_is_uptodate(pcp)) {
                if (page_cache_fill_vfile_page(vf, pcp) < 0) {
                    page_cache_put(pcp);
                    vfs_put_file_ref(vma->file_fd, vf);
                    return -1;
                }
            }
            vfs_put_file_ref(vma->file_fd, vf);

            pfn_t pfn = page_cache_pfn(pcp);
            if (!pfn_valid(pfn)) {
                page_cache_put(pcp);
                return -1;
            }

            if (vma->vm_flags & VM_SHARED) {
                frame_get(pfn);
                int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                               vma->pte_flags);
                page_cache_put(pcp);
                if (r < 0) {
                    frame_put(pfn);
                    return -1;
                }
            } else {
                if (cg_mem_charge(t->cgroup, 1) != 0) {
                    page_cache_put(pcp);
                    cg_mem_oom_kill(t->cgroup);
                    return -1;
                }
                pfn_t copy = pfa_alloc_page();
                if (copy == PFN_NONE) {
                    cg_mem_uncharge(t->cgroup, 1);
                    page_cache_put(pcp);
                    return -1;
                }
                memcpy(pfn_to_virt(copy), page_cache_data(pcp), PAGE_SIZE);
                page_cache_put(pcp);
                int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(copy),
                               vma->pte_flags);
                if (r < 0) {
                    cg_mem_uncharge(t->cgroup, 1);
                    frame_put(copy);
                    return -1;
                }
            }

            t->mm->rss++;
            arch_tlb_flush_page(stval);
            return 0;
        }

        if ((vma->vm_flags & VM_VMO) && vma->vmo) {
            uint32_t pg_idx = (uint32_t)((vma->vmo_offset + (page_va - vma->start)) / PAGE_SIZE);
            pfn_t vpfn = a20_vmo_get_page(vma->vmo, pg_idx);
            if (vpfn == PFN_NONE) return -1;

            int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(vpfn),
                            vma->pte_flags);
            if (r < 0) return -1;

            t->mm->rss++;
            arch_tlb_flush_page(stval);
            return 0;
        }

        if (!t->policy.thp_disabled && (vma->vm_flags & VM_HUGEPAGE) &&
            !(vma->vm_flags & VM_NOHUGEPAGE)) {
            uint64_t hbase = page_va & ~(uint64_t)(PMD_SIZE - 1);
            if (hbase >= vma->start && hbase + PMD_SIZE <= vma->end &&
                !pt_lookup_leaf(t->mm->pgdir, hbase, NULL, NULL, NULL)) {
                pfn_t hpfn = pfa_alloc(PMD_ORDER);
                if (hpfn != PFN_NONE) {
                    if (cg_mem_charge(t->cgroup, PMD_PAGE_COUNT) != 0) {
                        frame_put(hpfn);
                        cg_mem_oom_kill(t->cgroup);
                        return -1;
                    }
                    memset(pfn_to_virt(hpfn), 0, PMD_SIZE);
                    int hr = pt_map_huge(t->mm->pgdir, hbase, pfn_to_phys(hpfn),
                                         vma->pte_flags);
                    if (hr == 0) {
                        t->mm->rss += PMD_PAGE_COUNT;
                        arch_tlb_flush_page(stval);
                        return 0;
                    }
                    cg_mem_uncharge(t->cgroup, PMD_PAGE_COUNT);
                    frame_put(hpfn);
                }
            }
        }

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -1;
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            frame_put(pfn);
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                        vma->pte_flags);
        if (r < 0) { cg_mem_uncharge(t->cgroup, 1); frame_put(pfn); return -1; }

        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }

    return -1;
}
