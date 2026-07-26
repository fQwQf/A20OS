#include "mm/fault.h"

#include "proc/proc.h"
#include "proc/signal.h"
#include "core/signal_defs.h"
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
#include "mm/swap.h"

/*
 * COW_FAULT_TLB_CONTRACT:
 * - handle_cow_fault() updates the PTE before dropping the old frame reference.
 * - A page TLB flush follows every PTE replacement before returning to user.
 * - The mm_fork() regression guard depends on this ordering when a parent thread
 *   faults while another thread forks the same mm.
 */
/*
 * MAP_SHARED_FILE_CACHE_CONTRACT:
 * - Shared file mappings use the canonical page-cache page as the backing frame.
 * - page_cache_get() pins the page; the pin is released by page_cache_put() when
 *   the mapping is unmapped, moved, or torn down.
 * - The page-cache page therefore owns writeback: user writes through the mapped
 *   PTE update page-cache data directly; fsync/msync mark the page dirty via
 *   PTE_D scanning and then write it back through vnode->ops->writepage.
 * - Read() on the same file uses the same page cache, so shared mmap writes are
 *   visible to read() without an explicit sync.
 */
int mm_shared_file_fault(mm_struct_t *mm, vm_area_t *vma, uint64_t page_va,
                         vfile_t *vf)
{
    if (!mm || !vma || !vf || !vf->vnode)
        return -1;

    uint64_t file_pos = vma->file_offset + (page_va - vma->start);
    if (file_pos >= vf->vnode->size) {
        signal_send(proc_current()->pid, SIGBUS);
        return -1;
    }

    uint64_t index = file_pos / PAGE_SIZE;
    page_cache_page_t *pcp = page_cache_get(vf->vnode, index, 1);
    if (!pcp)
        return -1;

    if (!page_cache_is_uptodate(pcp)) {
        if (page_cache_fill_vfile_page(vf, pcp) < 0) {
            page_cache_put(pcp);
            return -1;
        }
    }

    pfn_t cache_pfn = page_cache_pfn(pcp);
    if (!pfn_valid(cache_pfn)) {
        page_cache_put(pcp);
        return -1;
    }

    if (vma->pte_flags & PTE_X)
        arch_flush_icache_range(page_cache_data(pcp), PAGE_SIZE);
    int r = pt_map(mm->pgdir, page_va, pfn_to_phys(cache_pfn), vma->pte_flags);
    if (r < 0) {
        page_cache_put(pcp);
        return -1;
    }

    mm->rss++;
    arch_tlb_flush_page(page_va);
    return 0;
}

static int handle_cow_fault_locked(task_t *t, uint64_t stval) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    return -1;
#else
    if (!t->mm || !t->mm->pgdir) return -1;

    vaddr_t leaf_base = 0;
    size_t leaf_size = 0;
    pte_t *pte = pt_lookup_leaf(t->mm->pgdir, stval, NULL, &leaf_base, &leaf_size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte) || !(*pte & PTE_U))
        return -1;

    if (*pte & PTE_COW) {
        paddr_t old_pa = arch_pte_addr(*pte);
        pfn_t old_pfn = phys_to_pfn(old_pa);
        if (!pfn_valid(old_pfn)) return -1;
        int order = (leaf_size >= PMD_SIZE) ? PMD_ORDER : 0;

        pfn_t new_pfn = PFN_NONE;

        /* 在 pfa.lock 内完成引用计数操作和 reuse 决策。
         * 对于 reuse（独占页）的情况，在释放锁之前就更新 PTE，
         * 防止定时器中断调度其他任务导致同一页面被 frame_put 释放后
         * 被 buddy 回收并分配给用户数据（0x63636363 损坏的根因）。 */
        uint64_t flags = (*pte & (PTE_R | PTE_X | PTE_U | PTE_A |
                                  PTE_G | PTE_MAT1 | PTE_LEAF)) |
                         PTE_W | PTE_D;

        uint64_t pfa_flags = spin_lock_irqsave(&pfa.lock);
        uint16_t rc = pfa.meta[old_pfn].refcount;
        if (rc == 0) {
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            new_pfn = pfa_alloc(order);
            if (new_pfn == PFN_NONE)
                return -1;
            memset(pfn_to_virt(new_pfn), 0, leaf_size);
            *pte = arch_pte_leaf(pfn_to_phys(new_pfn), flags);
            arch_tlb_flush_page(stval);
            return 0;
        }
        if (rc > 1) {
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);

            new_pfn = pfa_alloc(order);
            if (new_pfn == PFN_NONE)
                return -1;

            memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), leaf_size);

            /* 在 frame_put 之前更新 PTE，防止以下竞争：
             * 两个任务同时对同一物理页做 COW fault，都读到 rc>1 并释放锁。
             * 如果先 frame_put 再更新 PTE，第二次 frame_put 可能使引用
             * 计数归零并释放页面，而 PTE 仍指向已释放的物理页。
             * 该页面被 buddy 回收后可能立刻分配给 slab 或用户数据（产生
             * 0x63636363 损坏），TLB fill 走查过期 PTE 时读取到损坏内容。
             * 先更新 PTE 再 frame_put，确保 PTE 不再引用旧页后才释放。 */
            *pte = arch_pte_leaf(pfn_to_phys(new_pfn), flags);
            arch_tlb_flush_page(stval);

            frame_put(old_pfn);
            return 0;
        } else {
            *pte = arch_pte_leaf(old_pa, flags);
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            arch_tlb_flush_page(stval);
            return 0;
        }
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
#endif
}

/*
 * Demand paging + stack growth.
 *
 * Called for page faults after COW has been ruled out or was not applicable.
 * Handles lazy stack growth, brk pages, and anonymous VMA pages.
 */
/*
 * DEMAND_FAULT_TLB_CONTRACT:
 * - Stack/brk/anonymous/file/VMO/huge-page demand faults install a PTE, update
 *   rss/accounting, then flush the faulting page before returning.
 * - File-backed private faults copy from page cache; shared faults currently
 *   read through the file into a private frame and therefore do not yet provide
 *   full MAP_SHARED dirty/writeback coherence.
 */
static int handle_demand_fault_locked(task_t *t, uint64_t stval) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    return -1;
#else
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
    pte_t *pte = pt_lookup_leaf(t->mm->pgdir, page_va, NULL, NULL, NULL);

#ifdef CONFIG_SWAP
    if (pte && pte_is_swap(*pte)) {
        vm_area_t *vma = mm_find_vma(t->mm, page_va);
        if (!vma) {
            signal_send(t->pid, SIGBUS);
            return -1;
        }

        swap_entry_t entry = pte_to_swp_entry(*pte);
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE)
            return -1;
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            frame_put(pfn);
            return -ENOMEM;
        }
        if (swap_read_page(entry, pfn_to_virt(pfn)) < 0) {
            cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            return -1;
        }

        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                       vma->pte_flags);
        if (r < 0) {
            cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            return -1;
        }

        swap_free(entry);
        cg_mem_swap_uncharge(t, 1);
        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }
#endif

    if (t->mm->stack_top != 0) {
        uint64_t stack_size_limit = t->limits.stack ? t->limits.stack : USER_STACK_MAX_SIZE;
        if (stack_size_limit > USER_STACK_MAX_SIZE)
            stack_size_limit = USER_STACK_MAX_SIZE;
        stack_size_limit = ROUND_UP(stack_size_limit, PAGE_SIZE);
        uint64_t stack_limit = t->mm->stack_top - stack_size_limit;
        if (page_va >= stack_limit && page_va < t->mm->stack_top) {
            pte_t *pte = pt_walk(t->mm->pgdir, page_va, 0);
            if (pte && (*pte & PTE_V))
                return -1;

            pfn_t pfn = pfa_alloc_page();
            if (pfn == PFN_NONE) return -1;
            if (cg_mem_charge(t->cgroup, 1) != 0) {
                frame_put(pfn);
                return -ENOMEM;
            }
            memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

            int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                           mm_user_stack_pte_flags());
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
            return -ENOMEM;
        }
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) { cg_mem_uncharge(t->cgroup, 1); return -1; }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = pt_map(t->mm->pgdir, page_va, pfn_to_phys(pfn),
                       mm_user_brk_pte_flags());
        if (r < 0) { cg_mem_uncharge(t->cgroup, 1); frame_put(pfn); return -1; }

        t->mm->rss++;
        arch_tlb_flush_page(stval);
        return 0;
    }

    vm_area_t *vma = mm_find_vma(t->mm, page_va);
    if (vma) {
        if (pte && (*pte & PTE_V)) return -1;
        if (!mm_pte_flags_allow_access(vma->pte_flags)) return -1;

        if ((vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
            vfile_t *vf = vfs_get_file_ref(vma->file_fd);
            if (!vf)
                return -1;
            if (!vf->vnode) {
                vfs_put_file_ref(vma->file_fd, vf);
                return -1;
            }

            uint64_t file_pos = vma->file_offset + (page_va - vma->start);
            if (file_pos >= vf->vnode->size) {
                signal_send(t->pid, SIGBUS);
                vfs_put_file_ref(vma->file_fd, vf);
                return -1;
            }

            if (vma->vm_flags & VM_SHARED) {
                int r = mm_shared_file_fault(t->mm, vma, page_va, vf);
                vfs_put_file_ref(vma->file_fd, vf);
                return r;
            } else {
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

                pfn_t cache_pfn = page_cache_pfn(pcp);
                if (!pfn_valid(cache_pfn)) {
                    page_cache_put(pcp);
                    return -1;
                }

                if (cg_mem_charge(t->cgroup, 1) != 0) {
                    page_cache_put(pcp);
                    return -ENOMEM;
                }
                pfn_t copy = pfa_alloc_page();
                if (copy == PFN_NONE) {
                    cg_mem_uncharge(t->cgroup, 1);
                    page_cache_put(pcp);
                    return -1;
                }
                memcpy(pfn_to_virt(copy), page_cache_data(pcp), PAGE_SIZE);
                if (vma->pte_flags & PTE_X)
                    arch_flush_icache_range(pfn_to_virt(copy), PAGE_SIZE);
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

        if (!t->policy.thp_disabled && !vma->file_vnode &&
            (vma->vm_flags & VM_HUGEPAGE) &&
            !(vma->vm_flags & VM_NOHUGEPAGE)) {
            uint64_t hbase = page_va & ~(uint64_t)(PMD_SIZE - 1);
            if (hbase >= vma->start && hbase + PMD_SIZE <= vma->end &&
                !pt_lookup_leaf(t->mm->pgdir, hbase, NULL, NULL, NULL)) {
                pfn_t hpfn = pfa_alloc(PMD_ORDER);
                if (hpfn != PFN_NONE) {
                    if (cg_mem_charge(t->cgroup, PMD_PAGE_COUNT) != 0) {
                        frame_put(hpfn);
                        return -ENOMEM;
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
            return -ENOMEM;
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
#endif
}

#ifndef CONFIG_NOMMU
static int handle_file_fault(task_t *t, uint64_t page_va, int file_fd,
                             uint64_t file_pos, int shared, vfile_t *vf)
{
    if (file_pos >= vf->vnode->size) {
        signal_send(t->pid, SIGBUS);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }
    if (!vf->vnode->ops || !vf->vnode->ops->readpage) {
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    page_cache_page_t *pcp = page_cache_get(vf->vnode,
                                             file_pos / PAGE_SIZE, 1);
    if (!pcp) {
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }
    int fill_r = 0;
    if (!page_cache_is_uptodate(pcp))
        fill_r = page_cache_fill_vfile_page(vf, pcp);
    if (fill_r < 0) {
        page_cache_put(pcp);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }
    if (file_pos >= vf->vnode->size) {
        signal_send(t->pid, SIGBUS);
        page_cache_put(pcp);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    pfn_t candidate = page_cache_pfn(pcp);
    int charged = 0;
    if (!pfn_valid(candidate)) {
        page_cache_put(pcp);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    if (!shared) {
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            page_cache_put(pcp);
            vfs_put_file_ref(file_fd, vf);
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        charged = 1;
        candidate = pfa_alloc_page();
        if (candidate == PFN_NONE) {
            cg_mem_uncharge(t->cgroup, 1);
            page_cache_put(pcp);
            vfs_put_file_ref(file_fd, vf);
            return -1;
        }
        memcpy(pfn_to_virt(candidate), page_cache_data(pcp), PAGE_SIZE);
        page_cache_put(pcp);
        pcp = NULL;
    }

    mm_struct_t *mm = t->mm;
    uint64_t mm_flags = spin_lock_irqsave(&mm->lock);
    vm_area_t *vma = mm_find_vma(mm, page_va);
    vfile_t *current_vf = vma && (vma->vm_flags & VM_FILE) &&
                          vma->file_fd >= 0
        ? vfs_get_file_ref(vma->file_fd) : NULL;
    int mapping_valid = vma && current_vf && current_vf->vnode == vf->vnode &&
        (vma->vm_flags & VM_FILE) &&
        mm_pte_flags_allow_access(vma->pte_flags) &&
        !!(vma->vm_flags & VM_SHARED) == !!shared &&
        vma->file_fd == file_fd &&
        vma->file_offset + (page_va - vma->start) == file_pos;
    if (current_vf)
        vfs_put_file_ref(vma->file_fd, current_vf);

    int result = -1;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
    if (mapping_valid && pte && (*pte & PTE_V)) {
        result = 0;
    } else if (mapping_valid &&
               pt_map(mm->pgdir, page_va, pfn_to_phys(candidate),
                      vma->pte_flags) == 0) {
        mm->rss++;
        arch_tlb_flush_page(page_va);
        result = 0;
        candidate = PFN_NONE;
        if (shared)
            pcp = NULL; /* The mapping retains the page-cache pin. */
    }
    spin_unlock_irqrestore(&mm->lock, mm_flags);

    if (candidate != PFN_NONE && !shared)
        frame_put(candidate);
    if (pcp)
        page_cache_put(pcp);
    if (charged && candidate != PFN_NONE)
        cg_mem_uncharge(t->cgroup, 1);
    vfs_put_file_ref(file_fd, vf);
    return result;
}
#endif

int handle_cow_fault(task_t *t, uint64_t stval)
{
#ifdef CONFIG_NOMMU
    return handle_cow_fault_locked(t, stval);
#else
    if (!t || !t->mm)
        return -1;
    mm_struct_t *mm = t->mm;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = handle_cow_fault_locked(t, stval);
    spin_unlock_irqrestore(&mm->lock, flags);
    return r;
#endif
}

int handle_demand_fault(task_t *t, uint64_t stval)
{
#ifdef CONFIG_NOMMU
    return handle_demand_fault_locked(t, stval);
#else
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
#ifdef CONFIG_SWAP
    if (pte && pte_is_swap(*pte)) {
        /* Swap I/O cannot run under the IRQ-disabling mm spinlock.  A future
         * busy swap PTE will close the remaining duplicate-swapin race. */
        spin_unlock_irqrestore(&mm->lock, flags);
        int r = handle_demand_fault_locked(t, stval);
        if (r == -ENOMEM) {
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        return r;
    }
#endif
    if (pte && (*pte & PTE_V)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        return -1;
    }
    vm_area_t *vma = mm_find_vma(mm, page_va);
    if (vma && (vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
        if (!mm_pte_flags_allow_access(vma->pte_flags)) {
            spin_unlock_irqrestore(&mm->lock, flags);
            return -1;
        }
        int file_fd = vma->file_fd;
        int shared = (vma->vm_flags & VM_SHARED) != 0;
        uint64_t file_pos = vma->file_offset + (page_va - vma->start);
        vfile_t *vf = vfs_get_file_ref(file_fd);
        spin_unlock_irqrestore(&mm->lock, flags);
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file_ref(file_fd, vf);
            return -1;
        }
        return handle_file_fault(t, page_va, file_fd, file_pos, shared, vf);
    }

    int r = handle_demand_fault_locked(t, stval);
    spin_unlock_irqrestore(&mm->lock, flags);
    if (r == -ENOMEM) {
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }
    return r;
#endif
}
