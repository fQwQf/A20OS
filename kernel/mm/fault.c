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
#include "mm/pt.h"
#include "mm/vmo.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/string.h"
#include "cg/cgroup.h"
#include "mm/swap.h"
#include "ipc/userfaultfd.h"

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
 *   Both shared file-fault entry points do this: handle_file_fault() installs
 *   page_cache_pfn() directly (the `shared` candidate branch), and
 *   mm_shared_file_fault() below maps cache_pfn from page_cache_get().  The
 *   dispatch that selects between the private and shared file paths is the
 *   VM_FILE/VM_SHARED test in handle_demand_fault_access() plus the mirror in
 *   handle_demand_fault_locked().
 * - page_cache_get() pins the page; the pin is released by page_cache_put() when
 *   the mapping is unmapped, moved, or torn down.  handle_file_fault() transfers
 *   the pin to the mapping by clearing its window[] slot.
 * - The page-cache page therefore owns writeback: user writes through the mapped
 *   PTE update page-cache data directly; fsync/msync mark the page dirty via
 *   PTE_D scanning and then write it back through vnode->ops->writepage.
 *   The scan is mm_sync_shared_dirty_for_vnode() in mm/mmap.c, reached from
 *   vfs_fsync_vfile() and sys_msync() (and from the read path, so a stale
 *   !uptodate refill cannot overwrite newer mmap data); the writeback is
 *   page_cache_writeback_vnode() falling through to vnode->ops->writepage.
 * - Read() on the same file uses the same page cache, so shared mmap writes are
 *   visible to read() without an explicit sync.
 * - Boundary: this contract covers MAP_SHARED *file* mappings only.  It says
 *   nothing about anonymous MAP_SHARED (no VM_FILE, no page-cache identity) or
 *   about MAP_PRIVATE, whose COW conversion is specified at
 *   handle_cow_fault_locked() below.
 */
/*
 * Install one mapping through the transactional cursor.  Every fault-path PTE
 * write goes through here, so the per-PTE status and the hardware entry are
 * always updated in the same step and no path can update one without the
 * other.  A return of 1 from mm_addrspace_lock means a larger-than-needed
 * leaf already covers the address, which a leaf map must not silently ignore.
 */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static int fault_map(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                     uint8_t cls)
{
    mm_cursor_t cur;
    int r = mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &cur);
    if (r != 0)
        return r < 0 ? r : -EFAULT;
    r = mm_cursor_map(&cur, page_va, pfn_to_phys(pfn), flags, cls);
    mm_cursor_unlock(&cur);
    return r;
}
#else
static int fault_map(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                     uint8_t cls)
{
    (void)cls;
    return pt_map(mm->pgdir, page_va, pfn_to_phys(pfn), flags);
}
#endif

/*
 * Install a whole fault-around window [start, start + n*PAGE_SIZE), capped at
 * `end`.  Same shape as fault_map() and the same reason for existing: the
 * cursor API is not available to every architecture, so the two variants live
 * side by side here instead of the caller reaching for mm_addrspace_lock()
 * directly.  Only the number of page-table descents differs between them -- a
 * cursor build takes one covering-node lock and one descent for the range, a
 * non-cursor build walks once per page through that architecture's own pt_map().
 * The mappings, the status byte, rss and the perf counters are the same either
 * way, so an architecture without a transactional backend loses the
 * single-descent optimisation and nothing else.
 *
 * Returns the number of pages installed, or a negative errno if the range
 * could not be locked at all.  A short count is reported rather than assumed,
 * because the caller releases the frames of the pages this did not install and
 * must not release one it did.
 *
 * This is the arm32 case in particular: kernel/arch/arm32/mm/pgtbl.c supplies a
 * short-descriptor backend with no cursor and no per-PTE status sidecar, and
 * Makefile:866-869 withholds ARCH_HAS_PGTABLE_OPS from it, so
 * kernel/include/mm/pt.h never declares mm_addrspace_lock() there.  Calling it
 * from the fault path is what broke check-arm32-bringup; this hook is the
 * supported way in instead.
 */
#if !defined(CONFIG_NOMMU)
#if defined(ARCH_HAS_PGTABLE_OPS)
static int fault_map_window(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            const pfn_t *pfns, size_t n, pte_t flags)
{
    mm_cursor_t cur;
    int r = mm_addrspace_lock(mm, start, end, &cur);
    if (r != 0)
        return r < 0 ? r : -EFAULT;
    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        vaddr_t va = start + (vaddr_t)i * PAGE_SIZE;
        if (va >= end)
            break;
        if (mm_cursor_map(&cur, va, pfn_to_phys(pfns[i]), flags,
                          MM_ST_ANON_MAPPED) < 0)
            break;
        mapped++;
    }
    mm_cursor_unlock(&cur);
    return mapped;
}
#else
static int fault_map_window(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            const pfn_t *pfns, size_t n, pte_t flags)
{
    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        vaddr_t va = start + (vaddr_t)i * PAGE_SIZE;
        if (va >= end)
            break;
        if (pt_map(mm->pgdir, va, pfn_to_phys(pfns[i]), flags) < 0)
            break;
        mapped++;
    }
    return mapped;
}
#endif /* ARCH_HAS_PGTABLE_OPS */
#endif /* !CONFIG_NOMMU */

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
    if (!pcp) {
        kerr("[SHFAULT] cache_get failed pid=%d va=0x%lx fd=%d idx=%lu\n",
             proc_current()->pid, (unsigned long)page_va, vma->file_fd,
             (unsigned long)index);
        return -1;
    }

    if (!page_cache_is_uptodate(pcp)) {
        if (page_cache_fill_vfile_page(vf, pcp) < 0) {
            kerr("[SHFAULT] fill failed pid=%d va=0x%lx fd=%d idx=%lu\n",
                 proc_current()->pid, (unsigned long)page_va, vma->file_fd,
                 (unsigned long)index);
            page_cache_put(pcp);
            return -1;
        }
    }

    pfn_t cache_pfn = page_cache_pfn(pcp);
    if (!pfn_valid(cache_pfn)) {
        kerr("[SHFAULT] bad pfn pid=%d va=0x%lx fd=%d idx=%lu pfn=%lu\n",
             proc_current()->pid, (unsigned long)page_va, vma->file_fd,
             (unsigned long)index, (unsigned long)cache_pfn);
        page_cache_put(pcp);
        return -1;
    }

    if (vma->pte_flags & PTE_X)
        arch_flush_icache_range(page_cache_data(pcp), PAGE_SIZE);
    int r = fault_map(mm, page_va, cache_pfn, vma->pte_flags,
                      MM_ST_FILE_SHARED);
    if (r < 0) {
        kerr("[SHFAULT] map failed pid=%d va=0x%lx fd=%d idx=%lu r=%d\n",
             proc_current()->pid, (unsigned long)page_va, vma->file_fd,
             (unsigned long)index, r);
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

        /* Do the refcount work and the reuse decision inside pfa.lock.  In
         * the reuse (exclusive page) case the PTE is updated before the lock is
         * dropped: otherwise a timer interrupt can schedule another task which
         * frame_put()s the same page, buddy recycles it, and it is handed out as
         * user data -- the root cause of the 0x63636363 corruption. */
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
        /* The VM_SHARED test is NOT a denial of MAP_SHARED page-cache
         * coherence.  It only gates the private-leaf COW copy below: a
         * read-only MAP_PRIVATE fault-around leaf maps the canonical cache
         * frame (see direct_private in handle_file_fault()), so this store
         * must break that aliasing by cloning.  A MAP_SHARED leaf is writable
         * in place by contract and must not be cloned.  Shared leaves skip the
         * cache-page branch entirely and fall through to the refcount path
         * below, which leaves an rc>1 frame shared and makes rc==1 writable
         * without copying -- both correct for MAP_SHARED, because the frame
         * belongs to the page cache and its writes are the file's data. */
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

            /* Update the PTE before frame_put(), against this race: two tasks
             * take a COW fault on the same physical page, both read rc>1, and
             * both drop the lock.  If frame_put ran first, the second one could
             * take the refcount to zero and free the page while the PTE still
             * points at it.  Once buddy recycles that frame it may go straight
             * to slab or to user data -- the 0x63636363 corruption -- and a TLB
             * fill walking the stale PTE reads the corrupted contents.  Updating
             * the PTE first means the page is released only after nothing
             * references it any more. */
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
 * - MAP_SHARED file faults do NOT take a private copy: they install the
 *   canonical page-cache frame itself and inherit MAP_SHARED_FILE_CACHE_CONTRACT
 *   above (dirty-bit harvest in mm_sync_shared_dirty_for_vnode() plus
 *   page_cache_writeback_vnode()).  Only MAP_PRIVATE file faults copy out of the
 *   page cache, and a read-only MAP_PRIVATE fault-around leaf may additionally
 *   map the cache frame under the COW conditions described at handle_cow_fault().
 * - Anonymous MAP_SHARED is a different thing and is NOT covered by that
 *   contract: with VM_FILE clear it takes the plain MM_ST_ANON_MAPPED zero-page
 *   path below, so its pages carry no page-cache identity and no writeback
 *   route.  Inter-process shared memory is served by the VM_VMO path instead.
 */
static int handle_demand_fault_locked(task_t *t, uint64_t stval,
                                      enum mm_fault_access access,
                                      int lock_held) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    (void)access;
    (void)lock_held;
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

        int r = fault_map(t->mm, page_va, pfn, vma->pte_flags,
                          MM_ST_ANON_MAPPED);
        if (r < 0) {
            cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            return -1;
        }

        swap_free(entry);
        cg_mem_swap_uncharge(t, 1);
        t->mm->rss++;
        arch_tlb_flush_page_local(stval);
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
        return 0;
    }
#endif

    if (t->mm->stack_top != 0) {

        uint64_t stack_size_limit = t->limits.stack ? t->limits.stack : USER_STACK_MAX_SIZE;
        if (stack_size_limit > USER_STACK_MAX_SIZE)
            stack_size_limit = USER_STACK_MAX_SIZE;
        stack_size_limit = ROUND_UP(stack_size_limit, PAGE_SIZE);
        uint64_t stack_limit = t->mm->stack_top - stack_size_limit;
        /* Once ASLR has moved the stack top down, the lower bound for stack
         * growth moves down with it.  Clamp to USER_STACK_FLOOR so it can never
         * intrude into the fixed vDSO/vvar/TLS region below (see
         * mm/vdso_layout.h). */
        if (stack_limit < USER_STACK_FLOOR)
            stack_limit = USER_STACK_FLOOR;
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

            int r = fault_map(t->mm, page_va, pfn, mm_user_stack_pte_flags(),
                              MM_ST_ANON_MAPPED);
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

        int r = fault_map(t->mm, page_va, pfn, mm_user_brk_pte_flags(),
                          MM_ST_ANON_MAPPED);
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
            if (!vf) {
                kerr("[MFAULT] file_fd dead pid=%d va=0x%lx fd=%d flags=0x%lx\n",
                     t->pid, (unsigned long)page_va, vma->file_fd,
                     (unsigned long)vma->vm_flags);
                return -1;
            }
            if (!vf->vnode) {
                kerr("[MFAULT] no vnode pid=%d va=0x%lx fd=%d\n",
                     t->pid, (unsigned long)page_va, vma->file_fd);
                vfs_put_file_ref(vma->file_fd, vf);
                return -1;
            }

            uint64_t file_pos = vma->file_offset + (page_va - vma->start);
            if (file_pos >= vf->vnode->size) {
                kerr("[MFAULT] oob pid=%d va=0x%lx fd=%d pos=%lu size=%llu\n",
                     t->pid, (unsigned long)page_va, vma->file_fd,
                     (unsigned long)file_pos,
                     (unsigned long long)vf->vnode->size);
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
                int r = fault_map(t->mm, page_va, copy, vma->pte_flags,
                                  MM_ST_FILE_PRIVATE);
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


            if (fault_map(t->mm, page_va, vpfn, vma->pte_flags,
                          MM_ST_VMO) < 0)
                return -1;

            t->mm->rss++;
            arch_tlb_flush_page_local(stval);
            return 0;
        }

#ifndef ARCH_NO_PMD_LEAF
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
#endif

        /* Reserve a small forward window for private writable
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

            if (lock_held) {
                vma_get(vma);
                spin_unlock(&t->mm->lock);
            }

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

            pte_t map_flags = vma->pte_flags;
            if (lock_held) {
                spin_lock(&t->mm->lock);
                pte_t *cp = pt_lookup_leaf(t->mm->pgdir, page_va,
                                           NULL, NULL, NULL);
                if (prepared > 0 && cp && (*cp & PTE_V)) {
                    for (size_t i = 0; i < prepared; i++) {
                        cg_mem_uncharge(t->cgroup, 1);
                        frame_put(pfns[i]);
                    }
                    vma_put(t->mm, vma);
                    return 0;
                }
                if (prepared == 0) {
                    vma_put(t->mm, vma);
                } else if (mm_find_vma(t->mm, page_va) != vma) {
                    for (size_t i = 0; i < prepared; i++) {
                        cg_mem_uncharge(t->cgroup, 1);
                        frame_put(pfns[i]);
                    }
                    vma_put(t->mm, vma);
                    return -1;
                } else {
                    map_flags = vma->pte_flags;
                    size_t keep = 0;
                    for (size_t i = 0; i < prepared; i++) {
                        uint64_t va = page_va + (uint64_t)i * PAGE_SIZE;
                        pte_t *p = pt_lookup_leaf(t->mm->pgdir, va,
                                                  NULL, NULL, NULL);
                        if (p && (*p & PTE_V)) {
                            cg_mem_uncharge(t->cgroup, 1);
                            frame_put(pfns[i]);
                            continue;
                        }
                        pfns[keep++] = pfns[i];
                    }
                    prepared = keep;
                }
            }

            /* One transaction for the whole window: a single covering-node
             * lock and a single descent, instead of one page-table walk per
             * page.  This is the property the single-level model exists for.
             * fault_map_window() keeps that where a cursor exists and falls
             * back to a per-page pt_map() where one does not; the window
             * itself, and everything the caller does with the count, is
             * identical. */
            int wmap = fault_map_window(t->mm, page_va, end, pfns, prepared,
                                        map_flags);
            if (wmap > 0)
                mapped = (size_t)wmap;
            for (size_t i = mapped; i < prepared; i++) {
                cg_mem_uncharge(t->cgroup, 1);
                frame_put(pfns[i]);
            }
            if (lock_held)
                vma_put(t->mm, vma);
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

        int r = fault_map(t->mm, page_va, pfn, vma->pte_flags,
                          MM_ST_ANON_MAPPED);
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
        kerr("[HFF] no readpage pid=%d fd=%d shared=%d\n",
             t->pid, file_fd, shared);
        vfs_put_file_ref(file_fd, vf);
        return -1;
    }

    page_cache_page_t *window[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    size_t window_count = 1;
    window[0] = page_cache_get(vf->vnode, file_pos / PAGE_SIZE, 1);
    if (!window[0]) {
        kerr("[HFF] cache_get NULL pid=%d fd=%d pos=%lu shared=%d\n",
             t->pid, file_fd, (unsigned long)file_pos, shared);
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
        kerr("[HFF] fill fail pid=%d fd=%d pos=%lu shared=%d\n",
             t->pid, file_fd, (unsigned long)file_pos, shared);
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
     * complete fault-around window.  That is the hot ext4 parallel-build path.
     * Single-page backends such as the embedded FAT32 development image keep
     * executable mappings on anonymous copies, so an unrelated late text
     * fault cannot perturb page-cache pin accounting inside a running test.
     * LoongArch64 and x86_64 additionally keep ALL executable private leaves
     * on the anonymous-copy path: direct exec leaves can lose text PTEs under
     * parallel loader/fault lifetimes there (dynamic-loader SIGSEGVs). */
    int direct_private = !shared && fault_around &&
#ifdef CONFIG_X86_64
        !executable;
#else
        (!executable || vf->vnode->ops->readpages);
#endif
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
        kerr("[HFF] no candidate pid=%d fd=%d pos=%lu shared=%d window=%lu\n",
             t->pid, file_fd, (unsigned long)file_pos, shared,
             (unsigned long)window_count);
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
            if (pt_map_cls(mm->pgdir, va, pfn_to_phys(candidates[i]),
                           map_flags,
                           shared ? MM_ST_FILE_SHARED
                                  : MM_ST_FILE_PRIVATE) < 0)
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
        mm_tlb_shootdown_page(mm, stval);
    }
    if (old_pfn != PFN_NONE)
        frame_put(old_pfn);
    if (old_page) {
        /* Drop the temporary lookup and the direct mapping's retained pin
         * only after every CPU has discarded the old PTE. */
        page_cache_put(old_page);
        page_cache_put(old_page);
    }
    if (r == 0 && t) {
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
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
    return handle_demand_fault_locked(t, stval, access, 0);
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
        int r = handle_demand_fault_locked(t, stval, access, 0);
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

    /*
     * MM_AS_FAULT_FROM_STATUS -- the paper's fault handler (Fig. 8) decides
     * from per-PTE status alone: query() yields Status::PrivateAnon / Mapped /
     * Invalid, and PrivateAnon is mapped directly using the permissions
     * recorded at mmap.  No VMA is consulted, which is exactly what the paper
     * credits for its advantage over Linux -- "the time Linux spends in the
     * VMA" (§6.2).
     *
     * Only a range that mm_pt_provision_anon() marked as MM_ST_ANON_VIRT is
     * served here.  Anything else -- never provisioned (too large to provision
     * eagerly), already mapped, an intermediate node, a file/VMO mapping, a
     * huge leaf -- reports a different status and falls through to the
     * VMA-based path below unchanged, so no other behaviour is affected.
     *
     * FAULT_FROM_STATUS_ABSENT_WITHOUT_PGTABLE_OPS: this path needs the per-PTE
     * status sidecar, and that sidecar does not exist on every architecture, so
     * the whole block is compiled out when it is absent.  The capability is
     * reported as ABSENT there, not faked and not silently skipped:
     *   - arm32 is the case today.  Makefile:866-869 withholds
     *     ARCH_HAS_PGTABLE_OPS from it because it supplies its own
     *     short-descriptor backend (kernel/arch/arm32/mm/pgtbl.c), which is a
     *     plain pt_map()/pt_walk() walker: no mm_cursor_query(), and no
     *     metadata block to read a Status out of.  The status byte lives in the
     *     software metadata that mm_pt_node_init() allocates and
     *     mm_pt_note_present() maintains -- both in pt.c's guarded region -- so
     *     a short-descriptor PTE carries no class at all.  Provisioning
     *     (kernel/mm/mmap.c:193) is guarded the same way, so no arm32 leaf can
     *     ever be marked MM_ST_ANON_VIRT and the condition below is
     *     unsatisfiable by construction, not by accident.
     *   - What arm32 gets instead is the VMA-based path further down, which is
     *     the same path every other architecture takes for a non-provisioned
     *     range and which is correct on its own.  No correctness is lost.
     *   - What a reader can observe: /proc/a20/perf reports
     *     mm_fault_from_status and mm_anon_provisioned as 0 on such a build,
     *     which is the truth (the path does not exist, so it never runs) and
     *     the same value the default configuration already reports for
     *     eager provisioning being off (see the note on MM_ANON_PROV_DEFAULT
     *     in kernel/include/mm/pt.h).  Writing /proc/a20/anonprov on such a
     *     build returns -ENOSYS rather than accepting a cap nothing reads
     *     (mm_pt_set_anon_prov_max()), so the knob cannot look live when it is
     *     not.
     *   - What this is NOT allowed to become: widening the ARCH_HAS_PGTABLE_OPS
     *     guard in kernel/include/mm/pt.h so the declarations appear on an
     *     architecture with no transactional backend behind them.  A declared
     *     cursor that nothing implements is a fabricated capability, which is
     *     worse than a build break because it fails silently.  arm32 gets a
     *     real cursor path -- the per-PTE metadata threaded through its
     *     walker -- when someone implements it; until then it is absent.
     */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    {
        mm_cursor_t qcur;
        int qr = mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &qcur);
        if (qr == 0) {
            uint8_t cls_byte = 0;
            int already = mm_cursor_query(&qcur, page_va, &cls_byte, NULL);
            /* A userfaultfd registration over this entry must win: the fault
             * has to be parked for the handler, not satisfied here.  The mark is
             * per entry precisely so this decision needs no VMA.  The mark is
             * authoritative *here* and is not re-derived from the range list:
             * satisfying an ANON_VIRT entry returns from this function, so the
             * userfaultfd_range_present() call on the VMA path below never
             * runs for it.  That makes unregister obliged to clear the mark
             * only for pages no registration still covers (docs 10.59). */
            if (!already && MM_ST_GET_CLASS(cls_byte) == MM_ST_ANON_VIRT &&
                !mm_cursor_safe_test(&qcur, page_va, MM_SAFE_UFFD)) {
                /* Round-trip the recorded prot bits back to PTE flags: they
                 * were produced by mm_pt_prot_bits() from the same encoding,
                 * so the access check matches the VMA path exactly. */
                int prot = 0;
                if (cls_byte & MM_ST_PROT_R) prot |= 1;
                if (cls_byte & MM_ST_PROT_W) prot |= 2;
                if (cls_byte & MM_ST_PROT_X) prot |= 4;
                /* Ask the architecture for the flag set rather than assembling
                 * PTE bits here.  Every user mapping needs more than R/W/X:
                 * riscv64 needs PTE_U, x86_64 additionally needs PTE_LEAF and
                 * an explicit NX, and the helper also encodes the W=>R
                 * dependency.  A hand-rolled mask silently drops whichever of
                 * those this particular architecture happens to demand -- that
                 * is what produced the supervisor-only PTE on riscv64 (b) and
                 * the leaf-less PTE on x86_64. */
                pte_t allow = mm_prot_to_pte_flags(prot);
                if (mm_pte_flags_allow_access(allow)) {
                    pfn_t np = pfa_alloc_page();
                    if (np != PFN_NONE) {
                        if (cg_mem_charge(t->cgroup, 1) == 0) {
                            memset(pfn_to_virt(np), 0, PAGE_SIZE);
                            if (mm_cursor_map(&qcur, page_va, pfn_to_phys(np),
                                              allow, MM_ST_ANON_MAPPED) == 0) {
                                mm_cursor_unlock(&qcur);
                                mm->rss++;
                                a20_perf_count(A20_PERF_MM_ANON_FAULTS);
                                a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
                                a20_perf_count(A20_PERF_MM_FAULT_FROM_STATUS);
                                /* The per-task / global soft-fault counters the
                                 * VMA paths bump alongside rss++.  A fault path
                                 * that does not record its faults makes every
                                 * reader of these -- and /proc's reported fault
                                 * rate -- wrong. */
                                __atomic_fetch_add(&t->perf_page_faults, 1,
                                                   __ATOMIC_RELAXED);
                                __atomic_fetch_add(&t->perf_page_faults_maj, 1,
                                                   __ATOMIC_RELAXED);
                                __atomic_fetch_add(&g_perf_sw_page_faults, 1,
                                                   __ATOMIC_RELAXED);
                                __atomic_fetch_add(&g_perf_sw_page_faults_maj, 1,
                                                   __ATOMIC_RELAXED);
                                arch_tlb_flush_page_local(stval);
                                spin_unlock(&mm->lock);
                                return 0;
                            }
                            cg_mem_uncharge(t->cgroup, 1);
                        }
                        frame_put(np);
                    }
                }
            }
            mm_cursor_unlock(&qcur);
        }
    }
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

    vm_area_t *vma = mm_find_vma(mm, page_va);
    /*
     * USERFAULTFD_MISSING_HOOK: anonymous private ranges registered with a
     * userfaultfd hand the fault to the handler before the kernel fabricates
     * a zero page.  The mm lock is dropped before the handler parks so the
     * faulter can sleep; when the handler resolves the page (COPY/ZEROPAGE)
     * or the range is unregistered, the fault is retried from the top.
     */
    if (vma &&
        (vma->vm_flags & (VM_ANON | VM_FILE | VM_VMO | VM_SHARED)) == VM_ANON &&
        userfaultfd_range_present(mm, page_va)) {
        spin_unlock(&mm->lock);
        if (userfaultfd_handle_fault(t, mm, page_va,
                                     access == MM_FAULT_ACCESS_WRITE) < 0)
            return -1;
        return handle_demand_fault_access(t, stval, access);
    }
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
        int executable = (vma->pte_flags & PTE_X) != 0;
#ifdef CONFIG_LOONGARCH64
        /* LoongArch64 cannot yet retain private page-cache leaves safely
         * across the parallel loader/fault lifetime.  Keep private file pages
         * on the proven single-page copy path; direct executable leaves lose
         * text PTEs, while direct read-only leaves corrupt dynamic symbols in
         * librustc_driver under parallel compile load. */
        int fault_around = 0;
#else
        int fault_around = !shared && !(vma->pte_flags & PTE_W);
#endif
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
            __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
            t->perf_page_faults_maj++;
        }
        return r;
    }

    /*
     * PAGED-VMO hook: a VM_VMO mapping backed by a VMO_PAGED vmo with a
     * pager hands unmaterialized faults to the user-space pager.  The mm
     * lock is dropped before parking (mirrors the userfaultfd hook above);
     * vmo_paged_fault() loops until the pager supplies the page or the
     * pager disappears.
     */
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo &&
        vma->vmo->type == VMO_PAGED) {
        uint64_t voff = vma->vmo_offset + (page_va - vma->start);
        uint32_t pg_idx = (uint32_t)(voff / PAGE_SIZE);
        int paged_miss = 0;
        spin_lock(&vma->vmo->lock);
        if (pg_idx < vma->vmo->page_count &&
            vma->vmo->pages[pg_idx] == PFN_NONE && vma->vmo->pager)
            paged_miss = 1;
        spin_unlock(&vma->vmo->lock);
        if (paged_miss) {
            spin_unlock(&mm->lock);
            int pr = vmo_paged_fault(vma->vmo, pg_idx,
                                     access == MM_FAULT_ACCESS_WRITE);
            if (pr < 0)
                return -1;
            return handle_demand_fault_access(t, stval, access);
        }
    }

    int r = handle_demand_fault_locked(t, stval, access, 1);
    spin_unlock(&mm->lock);
    if (r == -ENOMEM) {
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }
    if (r == 0) {
        a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    }
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
