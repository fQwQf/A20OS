#include "mm/fault.h"
#include "mm/vm_internal.h"

#include "proc/proc.h"
#include "proc/signal.h"
#include "core/signal_defs.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/vmo.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/panic.h"
#include "core/string.h"
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
    arch_tlb_flush_page_local(page_va);
    return 0;
}

static int handle_cow_fault_locked(task_t *t, uint64_t stval,
                                   pfn_t *old_pfn_out,
                                   page_cache_page_t **old_page_out) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    (void)old_pfn_out;
    (void)old_page_out;
    return -1;
#else
    if (old_pfn_out)
        *old_pfn_out = PFN_NONE;
    if (old_page_out)
        *old_page_out = NULL;
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

        /* A private file page may still be the canonical page-cache frame.
         * Its allocator refcount describes cache ownership, not the number of
         * user mappings, so rc==1 must never make it writable in place. */
        vm_area_t *vma = mm_find_vma(t->mm, leaf_base);
        page_cache_page_t *cache_page =
            leaf_size == PAGE_SIZE
                ? mm_file_cache_mapping_get(vma, leaf_base, old_pfn)
                : NULL;
        if (cache_page && !(vma->vm_flags & VM_SHARED)) {
            new_pfn = pfa_alloc_page();
            if (new_pfn == PFN_NONE) {
                page_cache_put(cache_page);
                return -1;
            }
            memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), PAGE_SIZE);
            *pte = arch_pte_leaf(pfn_to_phys(new_pfn), flags);
            arch_tlb_flush_page_local(stval);
            if (old_page_out)
                *old_page_out = cache_page;
            else
                page_cache_put(cache_page);
            return 0;
        }
        if (cache_page)
            page_cache_put(cache_page);

        uint64_t pfa_flags = spin_lock_irqsave(&pfa.lock);
        uint16_t rc = pfa.meta[old_pfn].refcount;
        if (rc == 0) {
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            printf("[COW ZERO-REF] pid=%d va=0x%lx pfn=%lu order=%d\n",
                   t->pid, (unsigned long)stval, (unsigned long)old_pfn,
                   order);
            panic("COW leaf references a free frame");
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
            arch_tlb_flush_page_local(stval);

            /* Release only after the wrapper has completed the remote TLB
             * shootdown.  Otherwise a stale translation can write into this
             * frame after the buddy has already reused it. */
            if (old_pfn_out)
                *old_pfn_out = old_pfn;
            return 0;
        } else {
            *pte = arch_pte_leaf(old_pa, flags);
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            arch_tlb_flush_page_local(stval);
            return 0;
        }
        return 0;
    }

    if (*pte & PTE_W) {
        uint64_t flags = (*pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                                  PTE_G | PTE_A | PTE_MAT1 |
                                  PTE_LEAF | PTE_COW)) | PTE_D;
        *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
        arch_tlb_flush_page_local(stval);
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
static int handle_demand_fault_locked(task_t *t, uint64_t stval,
                                      enum mm_fault_access access) {
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
        arch_tlb_flush_page_local(stval);
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
            arch_tlb_flush_page_local(stval);
            return 0;
        }
    }

    if (page_va >= t->mm->start_brk &&
        page_va < ROUND_UP(t->mm->brk, PAGE_SIZE) &&
        !mm_find_vma(t->mm, page_va)) {
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
        a20_perf_count(A20_PERF_MM_ANON_FAULTS);
        arch_tlb_flush_page_local(stval);
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
            arch_tlb_flush_page_local(stval);
            return 0;
        }

        if ((vma->vm_flags & VM_VMO) && vma->vmo) {
            uint64_t voff = vma->vmo_offset + (page_va - vma->start);
            if (voff >= vma->vmo->size) {
                signal_send(t->pid, SIGBUS);
                return -1;
            }
            uint32_t pg_idx = (uint32_t)(voff / PAGE_SIZE);
            pfn_t vpfn;
            int r = vmo_get_page_charged(vma->vmo, pg_idx, t->cgroup, &vpfn);
            if (r == -ENOMEM)
                return -ENOMEM;
            if (r != 0 || vpfn == PFN_NONE)
                return -1;


            if (pt_map(t->mm->pgdir, page_va, pfn_to_phys(vpfn),
                       vma->pte_flags) < 0)
                return -1;

            t->mm->rss++;
            arch_tlb_flush_page_local(stval);
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
                        arch_tlb_flush_page_local(stval);
                        return 0;
                    }
                    cg_mem_uncharge(t->cgroup, PMD_PAGE_COUNT);
                    frame_put(hpfn);
                }
            }
        }

        /* Like MyGO, reserve a small forward window for private writable
         * anonymous store faults.  Compiler allocators usually touch new
         * arenas sequentially; installing four pages under one mm lock and
         * one fault return avoids three traps and repeated page-table walks.
         * Stack, shared, file, VMO and read-only mappings keep the single-page
         * path so speculative allocation cannot change their semantics. */
        if (access == MM_FAULT_ACCESS_WRITE &&
            (vma->vm_flags & (VM_ANON | VM_WRITE)) ==
                (VM_ANON | VM_WRITE) &&
            !(vma->vm_flags & (VM_SHARED | VM_STACK | VM_FILE | VM_VMO))) {
            enum { ANON_FAULT_AROUND_PAGES = 4 };
            pfn_t pfns[ANON_FAULT_AROUND_PAGES];
            size_t prepared = 0;
            size_t mapped = 0;
            uint64_t end = page_va +
                           ANON_FAULT_AROUND_PAGES * PAGE_SIZE;
            if (end < page_va || end > vma->end)
                end = vma->end;

            for (uint64_t va = page_va; va < end; va += PAGE_SIZE) {
                pte_t *next = pt_lookup_leaf(t->mm->pgdir, va,
                                             NULL, NULL, NULL);
                if (next && (*next & PTE_V))
                    break;
                pfn_t candidate = pfa_alloc_page();
                if (candidate == PFN_NONE)
                    break;
                if (cg_mem_charge(t->cgroup, 1) != 0) {
                    frame_put(candidate);
                    break;
                }
                memset(pfn_to_virt(candidate), 0, PAGE_SIZE);
                pfns[prepared++] = candidate;
            }

            for (size_t i = 0; i < prepared; i++) {
                uint64_t va = page_va + i * PAGE_SIZE;
                if (pt_map(t->mm->pgdir, va, pfn_to_phys(pfns[i]),
                           vma->pte_flags) < 0)
                    break;
                mapped++;
            }
            for (size_t i = mapped; i < prepared; i++) {
                cg_mem_uncharge(t->cgroup, 1);
                frame_put(pfns[i]);
            }
            if (mapped != 0) {
                t->mm->rss += mapped;
                a20_perf_count(A20_PERF_MM_ANON_FAULTS);
                a20_perf_count(A20_PERF_MM_ANON_BATCH_WINDOWS);
                a20_perf_add(A20_PERF_MM_ANON_BATCH_PAGES, mapped);
                arch_tlb_flush_page_local(stval);
                return 0;
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
        arch_tlb_flush_page_local(stval);
        return 0;
    }

    return -1;
#endif
}

#ifndef CONFIG_NOMMU
static uint64_t fault_file_size(vnode_t *vn)
{
    if (vn && vn->ops && vn->ops->stat) {
        kstat_t st;
        if (vn->ops->stat(vn, &st) == 0) {
            vn->size = st.st_size;
            return st.st_size;
        }
    }
    return vn ? vn->size : 0;
}

static int handle_file_fault(task_t *t, uint64_t page_va, int file_fd,
                             uint64_t file_pos, uint64_t vma_end,
                             int shared, int fault_around, int executable,
                             vfile_t *vf)
{
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }
    if (!vf->vnode->ops || !vf->vnode->ops->readpage) {
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    page_cache_page_t *window[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    size_t window_count = 1;
    window[0] = page_cache_get(vf->vnode, file_pos / PAGE_SIZE, 1);
    if (!window[0]) {
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    /* Read-only MAP_PRIVATE mappings can safely populate and install a small
     * forward window.  Each installed PTE still receives its own anonymous
     * copy, so a later mprotect()/write cannot modify the file cache. */
    if (fault_around && vf->vnode->ops->readpages && vma_end > page_va) {
        uint64_t vma_pages = (vma_end - page_va) / PAGE_SIZE;
        uint64_t file_bytes = vf->vnode->size - file_pos;
        uint64_t file_pages = (file_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t limit = vma_pages < file_pages ? vma_pages : file_pages;
        if (limit > PAGE_CACHE_FAULT_AROUND_PAGES)
            limit = PAGE_CACHE_FAULT_AROUND_PAGES;
        for (uint64_t i = 1; i < limit; i++) {
            page_cache_page_t *ahead = page_cache_get(
                vf->vnode, file_pos / PAGE_SIZE + i, 1);
            if (!ahead)
                break;
            window[window_count++] = ahead;
        }
    }

    int fill_r = 0;
    int needs_fill = 0;
    for (size_t i = 0; i < window_count; i++) {
        if (!page_cache_is_uptodate(window[i])) {
            needs_fill = 1;
            break;
        }
    }
    if (needs_fill) {
        if (window_count > 1)
            fill_r = page_cache_fill_vfile_pages(vf, window, window_count);
        else
            fill_r = page_cache_fill_vfile_page(vf, window[0]);

        /* Readahead is advisory.  A later-page error must not fail the
         * hardware fault if the requested page was published successfully. */
        if (fill_r < 0 && page_cache_is_uptodate(window[0]))
            fill_r = 0;
    }
    if (fill_r < 0) {
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    pfn_t candidates[PAGE_CACHE_FAULT_AROUND_PAGES];
    unsigned char charged[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    for (size_t i = 0; i < PAGE_CACHE_FAULT_AROUND_PAGES; i++)
        candidates[i] = PFN_NONE;

    /* Read-only MAP_PRIVATE leaves can use the same canonical cache frame as
     * MAP_SHARED.  A future mprotect(PROT_WRITE) marks such a leaf COW before
     * exposing write permission, so no eager anonymous copy is required. */
    /* Direct executable mappings are enabled for filesystems that can fill a
     * complete fault-around window.  That is the hot ext4 BuildStorm path.
     * Single-page backends such as the embedded FAT32 development image keep
     * executable mappings on anonymous copies, so an unrelated late text
     * fault cannot perturb page-cache pin accounting inside a running test. */
    int direct_private = !shared && fault_around &&
        (!executable || vf->vnode->ops->readpages);
    size_t candidate_count = shared ? 1 : window_count;
    for (size_t i = 0; i < candidate_count; i++) {
        if (!page_cache_is_uptodate(window[i]) ||
            !pfn_valid(page_cache_pfn(window[i]))) {
            candidate_count = i;
            break;
        }
        if (shared || direct_private) {
            candidates[i] = page_cache_pfn(window[i]);
            continue;
        }
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            candidate_count = i;
            break;
        }
        charged[i] = 1;
        candidates[i] = pfa_alloc_page();
        if (candidates[i] == PFN_NONE) {
            cg_mem_uncharge(t->cgroup, 1);
            charged[i] = 0;
            candidate_count = i;
            break;
        }
        memcpy(pfn_to_virt(candidates[i]), page_cache_data(window[i]),
               PAGE_SIZE);
    }

    if (candidate_count == 0) {
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file_ref(file_fd, vf);
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }

    mm_struct_t *mm = t->mm;
    spin_lock(&mm->lock);
    vm_area_t *vma = mm_find_vma(mm, page_va);
    vfile_t *current_vf = vma && (vma->vm_flags & VM_FILE) &&
                          vma->file_fd >= 0
        ? vfs_get_file_ref(vma->file_fd) : NULL;
    int mapping_valid = vma && current_vf && current_vf->vnode == vf->vnode &&
        (vma->vm_flags & VM_FILE) &&
        mm_pte_flags_allow_access(vma->pte_flags) &&
        !!(vma->pte_flags & PTE_X) == !!executable &&
        !!(vma->vm_flags & VM_SHARED) == !!shared &&
        vma->file_fd == file_fd &&
        vma->file_offset + (page_va - vma->start) == file_pos;
    if (current_vf)
        vfs_put_file_ref(vma->file_fd, current_vf);

    int result = -1;
    size_t installed = 0;
    if (mapping_valid) {
        size_t map_count = candidate_count;
        if (shared || (vma->pte_flags & PTE_W) || !fault_around)
            map_count = 1;
        for (size_t i = 0; i < map_count; i++) {
            uint64_t va = page_va + i * PAGE_SIZE;
            uint64_t pos = file_pos + i * PAGE_SIZE;
            if (va >= vma->end ||
                vma->file_offset + (va - vma->start) != pos)
                break;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
            if (pte && (*pte & PTE_V)) {
                if (i == 0)
                    result = 0;
                continue;
            }
            uint64_t map_flags = vma->pte_flags;
            if (direct_private)
                map_flags &= ~(uint64_t)(PTE_W | PTE_D | PTE_COW);
            if (direct_private && executable)
                arch_flush_icache_range(page_cache_data(window[i]),
                                        PAGE_SIZE);
            if (pt_map(mm->pgdir, va, pfn_to_phys(candidates[i]),
                       map_flags) < 0)
                break;
            mm->rss++;
            installed++;
            candidates[i] = PFN_NONE;
            charged[i] = 0;
            if (i == 0)
                result = 0;
            if (shared || direct_private)
                window[i] = NULL; /* Mapping retains the page-cache pin. */
        }
        if (installed > 1)
            arch_tlb_flush_local();
        else if (installed == 1 || result == 0)
            arch_tlb_flush_page_local(page_va);
    }
    spin_unlock(&mm->lock);

    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i] != PFN_NONE && !shared && !direct_private)
            frame_put(candidates[i]);
        if (charged[i])
            cg_mem_uncharge(t->cgroup, 1);
    }
    for (size_t i = 0; i < window_count; i++) {
        if (window[i])
            page_cache_put(window[i]);
    }
    vfs_put_file_ref(file_fd, vf);
    return result;
}
#endif

int handle_cow_fault(task_t *t, uint64_t stval)
{
#ifdef CONFIG_NOMMU
    return handle_cow_fault_locked(t, stval, NULL, NULL);
#else
    if (!t || !t->mm)
        return -1;
    mm_struct_t *mm = t->mm;
    pfn_t old_pfn = PFN_NONE;
    page_cache_page_t *old_page = NULL;
    spin_lock(&mm->lock);
    int r = handle_cow_fault_locked(t, stval, &old_pfn, &old_page);
    spin_unlock(&mm->lock);
    if (r == 0) {
        a20_perf_count(A20_PERF_MM_COW_FAULTS);
        arch_tlb_flush_page(stval);
    }
    if (old_pfn != PFN_NONE)
        frame_put(old_pfn);
    if (old_page) {
        /* Drop the temporary lookup and the direct mapping's retained pin
         * only after every CPU has discarded the old PTE. */
        page_cache_put(old_page);
        page_cache_put(old_page);
    }
    return r;
#endif
}

int handle_demand_fault(task_t *t, uint64_t stval)
{
    return handle_demand_fault_access(t, stval, MM_FAULT_ACCESS_READ);
}

int handle_demand_fault_access(task_t *t, uint64_t stval,
                               enum mm_fault_access access)
{
#ifdef CONFIG_NOMMU
    return handle_demand_fault_locked(t, stval, access);
#else
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
    spin_lock(&mm->lock);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
#ifdef CONFIG_SWAP
    if (pte && pte_is_swap(*pte)) {
        /* Swap I/O cannot run under the IRQ-disabling mm spinlock.  A future
         * busy swap PTE will close the remaining duplicate-swapin race. */
        spin_unlock(&mm->lock);
        int r = handle_demand_fault_locked(t, stval, access);
        if (r == -ENOMEM) {
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        return r;
    }
#endif
    if (pte && (*pte & PTE_V)) {
        spin_unlock(&mm->lock);
        return -1;
    }
    vm_area_t *vma = mm_find_vma(mm, page_va);
    if (vma && (vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
        if (!mm_pte_flags_allow_access(vma->pte_flags)) {
            spin_unlock(&mm->lock);
            return -1;
        }
        int file_fd = vma->file_fd;
        int shared = (vma->vm_flags & VM_SHARED) != 0;
        /* Writable private mappings stay on the single-page COW path.  A
         * read-only private mapping, including executable text, can share the
         * canonical page-cache frame.  mprotect(PROT_WRITE) converts the leaf
         * to COW before exposing writes, and unmap/exit drops the mapping's
         * cache pin.  This avoids allocating and copying the same rustc text
         * pages independently in every parallel compiler process. */
        int fault_around = !shared && !(vma->pte_flags & PTE_W);
        int executable = (vma->pte_flags & PTE_X) != 0;
        uint64_t vma_end = vma->end;
        uint64_t file_pos = vma->file_offset + (page_va - vma->start);
        vfile_t *vf = vfs_get_file_ref(file_fd);
        spin_unlock(&mm->lock);
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file_ref(file_fd, vf);
            return -1;
        }
        int r = handle_file_fault(t, page_va, file_fd, file_pos, vma_end,
                                  shared, fault_around, executable, vf);
        if (r == 0) {
            a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
            a20_perf_count(A20_PERF_MM_FILE_FAULTS);
        }
        return r;
    }

    int r = handle_demand_fault_locked(t, stval, access);
    spin_unlock(&mm->lock);
    if (r == -ENOMEM) {
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }
    if (r == 0)
        a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
    return r;
#endif
}

int handle_present_page_fault(task_t *t, uint64_t stval,
                              enum mm_fault_access access)
{
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    (void)access;
    return -1;
#else
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    spin_lock(&mm->lock);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, stval, NULL, NULL, NULL);
    int allowed = pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte) &&
                  (*pte & PTE_U);
    if (allowed) {
        if (access == MM_FAULT_ACCESS_WRITE)
            allowed = (*pte & PTE_W) != 0;
        else if (access == MM_FAULT_ACCESS_EXEC)
            allowed = (*pte & PTE_X) != 0;
        else
            allowed = (*pte & PTE_R) != 0;
    }
    /*
     * Radix-style MMUs take a reference/access (R/C) fault on a present
     * page whose R bit is clear.  Mark the leaf referenced so the retry
     * does not re-fault; the TLB flush below drops the stale entry.
     */
    if (allowed && pte)
        *pte |= PTE_A;
    spin_unlock(&mm->lock);

    if (!allowed)
        return -1;

    arch_tlb_flush_page_local(stval);
    return 0;
#endif
}
