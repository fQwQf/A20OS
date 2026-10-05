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
#include "core/errno.h"
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

/* COW breaks served by the lockless slice (mm_cow_from_status).  See
 * fault.h -- plain global so the shutdown audit can assert non-vacuity. */
uint64_t mm_cow_from_status_count;

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

int mm_shared_file_fault(mm_struct_t *mm, mm_seg_t *vma, uint64_t page_va,
                         vfile_t *vf)
{
    if (!mm || !vma || !vf || !vf->vnode)
        return -1;

    uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
    if (file_pos >= vf->vnode->size) {
        signal_send(proc_current()->pid, SIGBUS);
        return -1;
    }

    uint64_t index = file_pos / PAGE_SIZE;
    page_cache_page_t *pcp = page_cache_get(vf->vnode, index, 1);
    if (!pcp) {
        kerr("[SHFAULT] cache_get failed pid=%d va=0x%lx fd=%d idx=%lu\n",
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
             (unsigned long)index);
        return -1;
    }

    if (!page_cache_is_uptodate(pcp)) {
        if (page_cache_fill_vfile_page(vf, pcp) < 0) {
            kerr("[SHFAULT] fill failed pid=%d va=0x%lx fd=%d idx=%lu\n",
                 proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
                 (unsigned long)index);
            page_cache_put(pcp);
            return -1;
        }
    }

    pfn_t cache_pfn = page_cache_pfn(pcp);
    if (!pfn_valid(cache_pfn)) {
        kerr("[SHFAULT] bad pfn pid=%d va=0x%lx fd=%d idx=%lu pfn=%lu\n",
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
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
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
             (unsigned long)index, r);
        page_cache_put(pcp);
        return -1;
    }

    mm_rss_add(mm, 1);
    arch_tlb_flush_page_local(page_va);
    return 0;
}

/*
 * After a COW break has rewritten the PTE, bring the per-PTE status back in
 * step.  Every exit below clears PTE_COW and (usually) adds PTE_W, and all of
 * them used to leave the status describing the pre-fault page -- which is how
 * mm_pt_audit_all() ended up with a nonzero prot/cow mismatch, and how a
 * status-driven fault would re-install read-only over a page the process had
 * just been given write access to.
 *
 * `vma` is the mapping the fault was attributed to and is already resolved by
 * the caller; the class it implies is the backing, which COW does not change.
 */
#ifndef CONFIG_NOMMU
static void cow_sync_status(struct mm_struct *mm, vaddr_t va,
                            const mm_seg_t *vma)
{
    /* A COW break can also run on a huge leaf (the rc>1 branch copies the
     * whole 2 MiB frame).  The leaf's status slot then lives in its own
     * table at its own level, so both the table and the index must be taken
     * level-aware; a level-0 peek would read a neighbouring entry. */
    int level = 0;
    if (!pt_lookup_leaf(mm->pgdir, va, &level, NULL, NULL))
        return;
    pte_t *tab = mm_pt_leaf_table(mm->pgdir, va);
    if (!tab)
        return;
    int idx = arch_pt_vpn(va, level);

    /* Prefer the class the status already records; fall back to the VMA only
     * when there is none (a page whose status predates this path). */
    uint8_t cls = MM_ST_GET_CLASS(mm_pt_peek(tab, level, idx));
    if (cls == MM_ST_INVALID || cls == MM_ST_PT_NODE) {
        if (!vma)
            return;
        if (vma->vm_flags & VM_VMO)
            cls = MM_ST_VMO;
        else if (vma->vm_flags & VM_FILE)
            cls = (vma->vm_flags & VM_SHARED) ? MM_ST_FILE_SHARED
                                              : MM_ST_FILE_PRIVATE;
        else
            cls = MM_ST_ANON_MAPPED;
    }
    (void)mm_pt_sync_status(tab, level, idx, cls);
}
#endif /* CONFIG_NOMMU */

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
        mm_seg_t *vma = mm_seg_find(t->mm, leaf_base);
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
            cow_sync_status(t->mm, leaf_base, vma);
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
            cow_sync_status(t->mm, leaf_base, vma);
            arch_tlb_flush_page_local(stval);

            /* Release only after the wrapper has completed the remote TLB
             * shootdown.  Otherwise a stale translation can write into this
             * frame after the buddy has already reused it. */
            if (old_pfn_out)
                *old_pfn_out = old_pfn;
            return 0;
        } else {
            *pte = arch_pte_leaf(old_pa, flags);
            cow_sync_status(t->mm, leaf_base, vma);
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
        cow_sync_status(t->mm, leaf_base, mm_seg_find(t->mm, leaf_base));
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
/*
 * MM_FAULT_RETRY -- the fault-around window could not be installed because the
 * VMA that authorised it is no longer the object covering the address, and it
 * is not an error.
 *
 * The window deliberately drops mm->lock to allocate its frames, and while it
 * is unlocked a sibling thread sharing this mm can reshape the VMA list at that
 * address.  The commonest case is benign and happens constantly: mm_insert_vma()
 * coalesces two adjacent anonymous VMAs and keeps the NEW object, deferring the
 * old one, so a mapping nobody touched changes identity underneath the fault.
 * The address is still mapped, still writable, and still backed by an
 * equivalent VMA -- vma_can_merge() only merges equal vm_flags and pte_flags.
 *
 * Returning failure here turned that into SIGSEGV on a valid address, which is
 * how a four-thread process died on a store into memory it owned.  The window
 * gives its frames back and asks the caller to start over against whatever VMA
 * is current, which costs one more lookup and turns the race into a no-op.
 */
#define MM_FAULT_RETRY (-EAGAIN)

/* A window can only lose its VMA to a list mutation, and each retry resolves a
 * fresh one, so the bound is generous enough for a heavy mmap/munmap workload
 * and still finite: without it a pathological merger could spin here. */
#define MM_FAULT_RETRY_MAX 8

/* The definition sits behind #ifndef CONFIG_NOMMU, as does the only call site
 * (handle_demand_fault_access's non-NOMMU branch), so the declaration has to
 * carry the same guard -- otherwise a NOMMU build sees a static declaration
 * with no definition and -Werror takes the whole kernel down. */
#ifndef CONFIG_NOMMU
static int handle_demand_fault_attempt(task_t *t, uint64_t stval,
                                       enum mm_fault_access access);
#endif
/* ---- P6 shadow check (see docs/roadmap/single-level-mm-model.md 12.6) ----
 *
 * Asks the page-table segment table what it would have said about this fault,
 * and counts how often that matches what the mapping list said.  Changes
 * nothing: a non-zero disagreement count means the segment would have faulted
 * in the wrong page.
 *
 * Since the merge (roadmap 13.18) both sides resolve to the SAME mm_seg_t
 * record, so this no longer compares two representations -- it compares two
 * ways of resolving an address to one record, which can still differ when a
 * 2 MiB node entry names a neighbouring mapping.
 *
 * It lives here rather than in pt.c because only this side walks mm->mmap, and
 * a disagreement is only diagnosable when both sides are printed together.
 */
#ifndef CONFIG_NOMMU
static void shadow_seg_check(mm_struct_t *mm, mm_seg_t *vma, vaddr_t va,
                             uint8_t kind, uint64_t base_off)
{
    if (!(vma->vm_flags & (VM_VMO | VM_FILE)))
        return;
    if ((kind == MM_SEG_VMO) != ((vma->vm_flags & VM_VMO) != 0))
        return;

    int shared = (vma->vm_flags & VM_SHARED) ? 1 : 0;
    uint64_t want = base_off + (va - vma->start);
    mm_seg_t *found = NULL;
    if (mm_pt_shadow_seg(mm, va, kind, want, shared, &found) != 0 || !found)
        return;

    /* The mapping the page tables named, against the mapping that owns this
     * address right now.  They are different records when the annotation came
     * from a neighbour sharing this (much coarser) node entry, and the extents
     * are what are supposed to tell the two apart. */
    kwarn("[SEG-SHADOW] va=0x%lx: mapping [0x%lx,0x%lx) off=0x%lx shared=%d "
          "file=%lu says kind=%u, but the page tables name "
          "[0x%lx,0x%lx) off=0x%lx kind=%u shared=%d file=%lu%s\n",
          (unsigned long)va,
          (unsigned long)vma->start, (unsigned long)vma->end,
          (unsigned long)want, shared,
          (unsigned long)(vma->file ? vma->file->identity : 0), kind,
          (unsigned long)found->start, (unsigned long)found->end,
          (unsigned long)found->backing_offset, mm_seg_kind(found),
          mm_seg_shared(found),
          (unsigned long)(found->file ? found->file->identity : 0),
          vma == found ? "" : "  (a different mapping)");
    mm_seg_put(found);
}
#endif /* CONFIG_NOMMU */

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
        mm_seg_t *vma = mm_seg_find(t->mm, page_va);
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
        mm_rss_add(t->mm, 1);
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
            /*
             * A stack page is mapped read/write and never executable.  Faulting
             * one in to satisfy an *instruction* access would report success,
             * leave the leaf non-executable, and hand control back to a PC the
             * hardware cannot fetch from -- so the very next instruction
             * re-faults at the same address, forever.  That is exactly what a
             * jump through a corrupted function pointer looks like: mksh on
             * aarch64 spun on a prefetch abort at a stack address until the
             * timeout killed the machine, with no fault report, because every
             * round trip returned "handled".
             *
             * Refuse instead, so the caller reports SIGSEGV and kills the task.
             */
            if (access == MM_FAULT_ACCESS_EXEC)
                return -1;

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
            mm_rss_add(t->mm, 1);
            arch_tlb_flush_page_local(stval);
            return 0;
        }
    }

    if (page_va >= t->mm->start_brk &&
        page_va < ROUND_UP(t->mm->brk, PAGE_SIZE) &&
        !mm_seg_find(t->mm, page_va)) {
        /* Same reasoning as the stack branch: a heap page is never
         * executable, so an instruction fault here must not be satisfied with
         * a fresh read/write leaf or the fault repeats indefinitely. */
        if (access == MM_FAULT_ACCESS_EXEC)
            return -1;

        if (cg_mem_charge(t->cgroup, 1) != 0) {
            return -ENOMEM;
        }
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) { cg_mem_uncharge(t->cgroup, 1); return -1; }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = fault_map(t->mm, page_va, pfn, mm_user_brk_pte_flags(),
                          MM_ST_ANON_MAPPED);
        if (r < 0) { cg_mem_uncharge(t->cgroup, 1); frame_put(pfn); return -1; }

        mm_rss_add(t->mm, 1);
        a20_perf_count(A20_PERF_MM_ANON_FAULTS);
        arch_tlb_flush_page_local(stval);
        return 0;
    }

    mm_seg_t *vma = mm_seg_find(t->mm, page_va);
    if (vma) {

        if (pte && (*pte & PTE_V)) return -1;
        if (!mm_pte_flags_allow_access(vma->pte_flags)) return -1;

        /* Shadow check: would the segment table have answered this fault the
         * same way?  Nothing below changes behaviour -- it is the
         * measurement that says whether the page-table name and the mapping
         * list name the same record. */
        shadow_seg_check(t->mm, vma, page_va,
                         (vma->vm_flags & VM_VMO) ? MM_SEG_VMO : MM_SEG_FILE,
                         vma->backing_offset);

        if ((vma->vm_flags & VM_FILE) && vma->file) {
            vfile_t *vf = vma->file;
            vfile_get(vf);
            if (!vf->vnode) {
                kerr("[MFAULT] no vnode pid=%d va=0x%lx file=%p\n",
                     t->pid, (unsigned long)page_va, (void *)vf);
                vfs_put_file(vf);
                return -1;
            }

            uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
            if (file_pos >= vf->vnode->size) {
                kerr("[MFAULT] oob pid=%d va=0x%lx fd=%d pos=%lu size=%llu\n",
                     t->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
                     (unsigned long)file_pos,
                     (unsigned long long)vf->vnode->size);
                signal_send(t->pid, SIGBUS);
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                return -1;
            }

            if (vma->vm_flags & VM_SHARED) {
                int r = mm_shared_file_fault(t->mm, vma, page_va, vf);
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                return r;
            } else {
                page_cache_page_t *pcp = page_cache_get(vf->vnode,
                                                         file_pos / PAGE_SIZE, 1);
                if (!pcp) {
                    vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                    return -1;
                }
                if (!page_cache_is_uptodate(pcp)) {
                    if (page_cache_fill_vfile_page(vf, pcp) < 0) {
                        page_cache_put(pcp);
                        vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                        return -1;
                    }
                }
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);

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

            mm_rss_add(t->mm, 1);
            arch_tlb_flush_page_local(stval);
            return 0;
        }

        if ((vma->vm_flags & VM_VMO) && vma->vmo) {
            uint64_t voff = vma->backing_offset + (page_va - vma->start);
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

            mm_rss_add(t->mm, 1);
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
                    int hr = pt_map_huge(t->mm, hbase, pfn_to_phys(hpfn),
                                         vma->pte_flags, MM_ST_ANON_MAPPED);
                    if (hr == 0) {
                        a20_perf_count(A20_PERF_MM_HUGE_FAULTS);
                        mm_rss_add(t->mm, PMD_PAGE_COUNT);
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
                } else if (mm_seg_find(t->mm, page_va) != vma) {
                    for (size_t i = 0; i < prepared; i++) {
                        cg_mem_uncharge(t->cgroup, 1);
                        frame_put(pfns[i]);
                    }
                    vma_put(t->mm, vma);
                    return MM_FAULT_RETRY;
                } else {
                    map_flags = vma->pte_flags;
                    /* The window was sized from the VMA as it looked before
                     * the lock was dropped.  A concurrent munmap or a
                     * concurrent mprotect split can have shortened that same
                     * VMA since -- both lower vma->end in place rather than
                     * replacing the object -- so the span prepared above may
                     * now reach past the mapping.  Installing those pages
                     * anyway leaves present, zero-filled PTEs sitting in what
                     * is now a hole: the fault reports success, no thread ever
                     * wrote that memory, and the next mmap over the same
                     * address inherits the PTEs as if the application had
                     * faulted them in itself.  That is silent corruption, and
                     * it is the failure mode this window was suspected of
                     * causing, so the span is re-clamped here, under the lock,
                     * to the VMA's current end; the frames prepared beyond it
                     * are released by the short count fault_map_window()
                     * reports back. */
                    if (end > vma->end)
                        end = vma->end;
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
                mm_rss_add(t->mm, mapped);
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

        mm_rss_add(t->mm, 1);
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

static int handle_file_fault(task_t *t, uint64_t page_va,
                             uint64_t file_pos, uint64_t vma_end,
                             int shared, int fault_around, int executable,
                             vfile_t *vf)
{
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        vfs_put_file(vf);
        return -1;
    }
    if (!vf->vnode->ops || !vf->vnode->ops->readpage) {
        kerr("[HFF] no readpage pid=%d file=%lu shared=%d\n",
             t->pid, (unsigned long)vf->identity, shared);
        vfs_put_file(vf);
        return -1;
    }

    page_cache_page_t *window[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    size_t window_count = 1;
    window[0] = page_cache_get(vf->vnode, file_pos / PAGE_SIZE, 1);
    if (!window[0]) {
        kerr("[HFF] cache_get NULL pid=%d fd=%d pos=%lu shared=%d\n",
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared);
        vfs_put_file(vf);
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
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
        return -1;
    }
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
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
    int direct_private = !shared && fault_around && !executable;
    /* Backends with an explicit readpages hook are the only ones allowed to hand
     * a private executable leaf a shared page-cache frame, and only where the
     * architecture tolerates retaining one.  This used to be a CONFIG_X86_64 test
     * while the comment above claimed LoongArch64 behaved the same way; it now
     * does, and on LoongArch64 the distinction is unobservable anyway because
     * ARCH_FAULT_AROUND_UNSAFE already forces fault_around to 0. */
    if (!ARCH_EXE_LEAF_RETAIN_UNSAFE && vf->vnode->ops->readpages)
        direct_private = 1;
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
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared,
             (unsigned long)window_count);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }

    mm_struct_t *mm = t->mm;
    spin_lock(&mm->lock);
    mm_seg_t *vma = mm_seg_find(mm, page_va);
    vfile_t *current_vf = vma && (vma->vm_flags & VM_FILE) && vma->file
        ? vma->file : NULL;
    if (current_vf)
        vfile_get(current_vf);
    int mapping_valid = vma && current_vf && current_vf->vnode == vf->vnode &&
        (vma->vm_flags & VM_FILE) &&
        mm_pte_flags_allow_access(vma->pte_flags) &&
        !!(vma->pte_flags & PTE_X) == !!executable &&
        !!(vma->vm_flags & VM_SHARED) == !!shared &&
        vma->file == vf &&
        vma->backing_offset + (page_va - vma->start) == file_pos;
    if (current_vf)
        vfs_put_file(current_vf);

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
                vma->backing_offset + (va - vma->start) != pos)
                break;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
            if (pte && (*pte & PTE_V)) {
                if (i == 0)
                    result = 0;
                continue;
            }
            uint64_t map_flags = vma->pte_flags;
            if (direct_private) {
                /*
                 * Map the canonical page-cache frame read-only.  PTE_COW has
                 * to STAY: this leaf aliases a frame the page cache still
                 * owns, so the first store must copy rather than write
                 * through -- and PTE_COW is the only thing that routes the
                 * store to the copy in mm_fault_handle_cow(), which is
                 * written for exactly this leaf ("a read-only MAP_PRIVATE
                 * fault-around leaf maps the canonical cache frame, so this
                 * store must break that aliasing by cloning").
                 *
                 * Clearing it here left the leaf matching neither the COW
                 * branch nor the PTE_W dirty-bit branch, so the very first
                 * write to a private file page -- a .data/.bss store in
                 * ld-musl, for one -- fell through to "unhandled" and killed
                 * the process with SIGSEGV on a VMA the kernel itself
                 * considered writable.
                 */
                map_flags &= ~(uint64_t)(PTE_W | PTE_D);
                map_flags |= PTE_COW;
            }
            if (direct_private && executable)
                arch_flush_icache_range(page_cache_data(window[i]),
                                        PAGE_SIZE);
            if (pt_map_cls(mm->pgdir, va, pfn_to_phys(candidates[i]),
                           map_flags,
                           shared ? MM_ST_FILE_SHARED
                                  : MM_ST_FILE_PRIVATE) < 0)
                break;
            mm_rss_add(mm, 1);
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
    vfs_put_file(vf);
    return result;
}
#endif

/*
 * MM_COW_FROM_STATUS -- the lockless slice of the COW fault.
 *
 * A write fault whose leaf says class MM_ST_ANON_MAPPED with the COW bit is
 * a private anonymous page fork made shared; its private copy installs
 * without mm->lock, under the leaf lock alone:
 *   - the class answers "anonymous" (file pages need the page-cache branch
 *     and VMO pages answer to the VMO, so both decline here);
 *   - the PTE is re-verified as still COW/still this frame UNDER the leaf
 *     lock by mm_cursor_replace_if_cow(), which is the same lock fork's
 *     parent-side rewrite and mprotect's prot rewrite now take;
 *   - the refcount decision runs under pfa.lock, and the frame is pinned
 *     there, so reclaim cannot free it out from under the copy;
 *   - rc==1 declines: the in-place upgrade must exclude fork, which
 *     serialises under mm->lock.
 *
 * Copying a frame that turned out exclusive is a waste, never a bug: the
 * COW bit only means "was shared when we looked", and every sharer copies
 * or upgrades under its own exclusion.
 */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static int mm_cow_from_status(task_t *t, mm_struct_t *mm,
                              uint64_t page_va, uint64_t stval)
{
    mm_cursor_t cur;
    if (mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &cur) != 0)
        return 0;

    uint8_t cls_byte = 0;
    paddr_t old_pa = 0;
    int already = mm_cursor_query(&cur, page_va, &cls_byte, &old_pa);
    if (!already || MM_ST_GET_CLASS(cls_byte) != MM_ST_ANON_MAPPED ||
        !(cls_byte & MM_ST_COW_BIT) ||
        mm_cursor_safe_test(&cur, page_va, MM_SAFE_UFFD))
        goto decline;

    pfn_t old_pfn = phys_to_pfn(old_pa);
    if (!pfn_valid(old_pfn))
        goto decline;

    /* Refcount decision + pin under pfa.lock.  Node-then-pfa is the order
     * the cursor already establishes (mm_addrspace_lock holds node locks
     * while the caller allocates).  The pin is a manual refcount increment
     * rather than frame_get(): frame_get() takes pfa.lock itself, and
     * nesting it inside this critical section deadlocks on the very lock
     * the LOCK-STALL report pointed at.  The increment is exactly what
     * frame_get() does under the same lock, minus its zero-ref panic --
     * unreachable here, because rc>1 was read in the same critical
     * section. */
    uint64_t pfa_flags = spin_lock_irqsave(&pfa.lock);
    uint16_t rc = pfa.meta[old_pfn].refcount;
    if (rc <= 1) {
        /* rc==0 is the corrupted state the locked path panic-diagnoses;
         * rc==1 wants the in-place upgrade, which must exclude fork. */
        spin_unlock_irqrestore(&pfa.lock, pfa_flags);
        goto decline;
    }
    pfa.meta[old_pfn].refcount++;
    spin_unlock_irqrestore(&pfa.lock, pfa_flags);

    /* can_reclaim = 0, for the same reason the anon fast path insists on
     * it: a reclaiming allocation reaches oom_try_reclaim(), whose victim
     * teardown wants the page-table node locks this path holds. */
    pfn_t new_pfn = pfa_alloc_flags(0, 0);
    if (new_pfn == PFN_NONE) {
        frame_put(old_pfn);
        goto decline;
    }
    if (cg_mem_charge(t->cgroup, 1) != 0) {
        frame_put(new_pfn);
        frame_put(old_pfn);
        goto decline;
    }
    memcpy(pfn_to_virt(new_pfn), pfn_to_virt(old_pfn), PAGE_SIZE);

    paddr_t replaced = 0;
    int r = mm_cursor_replace_if_cow(&cur, page_va, old_pa,
                                     pfn_to_phys(new_pfn), MM_ST_ANON_MAPPED,
                                     &replaced);
    mm_cursor_unlock(&cur);
    if (r != 0) {
        /* State moved underneath us (fork rewrote it, another thread of
         * this mm broke the COW first, or the page was unmapped).  The
         * pinned frame is spare and so is the copy; the fault is resolved
         * either way -- the retried store either succeeds on the new state
         * or re-faults into the mm->lock path. */
        cg_mem_uncharge(t->cgroup, 1);
        frame_put(new_pfn);
        frame_put(old_pfn);
        return 1;
    }

    /* Remote shootdown BEFORE the old frame can be recycled: the pin holds
     * the frame alive until every CPU has dropped the stale translation
     * (the same contract handle_cow_fault's wrapper implements with
     * mm_tlb_hold_frame).  Two puts then release pin + our mapping's
     * reference. */
    mm_tlb_shootdown_page(mm, stval);
    frame_put(old_pfn);
    frame_put(old_pfn);

    a20_perf_count(A20_PERF_MM_COW_FAULTS);
    a20_perf_count(A20_PERF_MM_COW_FROM_STATUS);
    mm_cow_from_status_count++;    __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    return 1;

decline:
    mm_cursor_unlock(&cur);
    return 0;
}
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

int handle_cow_fault(task_t *t, uint64_t stval)
{
#ifdef CONFIG_NOMMU
    return handle_cow_fault_locked(t, stval, NULL, NULL);
#else
    if (!t || !t->mm)
        return -1;
    mm_struct_t *mm = t->mm;
#if defined(ARCH_HAS_PGTABLE_OPS)
    /* Lockless slice first, same shape as mm_fault_from_status(): a shared
     * private-anonymous COW leaf is broken under the leaf lock alone.  The
     * rc==1 in-place upgrade stays on the mm->lock path below -- making the
     * last reference writable has to exclude fork's frame_get, which
     * serialises under mm->lock, and skipping a copy that turns out
     * unneeded is an optimisation, not a correctness requirement. */
    if (mm_cow_from_status(t, mm, stval & ~(uint64_t)(PAGE_SIZE - 1), stval))
        return 0;
#endif
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

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
/* MM_AS_FAULT_FROM_STATUS -- the paper's fault handler (Fig. 8) decides from
 * per-PTE status alone: query() yields Status::PrivateAnon / Mapped / Invalid,
 * and PrivateAnon is mapped directly using the permissions recorded at mmap.  No
 * VMA is consulted, which is exactly what the paper credits for its advantage
 * over Linux -- "the time Linux spends in the VMA" (6.2).  That is also why
 * this runs BEFORE spin_lock(&mm->lock): it touches no VMA, so there is nothing
 * there for that lock to be protecting.
 *
 * Everything it does touch is cursor-owned or atomic: mm_rss_add() and the perf
 * counters were made atomic in Phase 0, cg_mem_charge() takes only its own
 * node->lock, and mm_cursor_query() reports a swap entry as already-present with
 * class MM_ST_SWAPPED, so the !already test declines swap without needing the
 * separate swap check the mm->lock path has to do.
 *
 * The page allocation is pfa_alloc_flags(0, 0) -- can_reclaim = 0 on purpose.  A
 * reclaiming allocator reaches oom_try_reclaim(), which swaps pages out and calls
 * proc_force_exit(); tearing the victim down runs pt_unmap_leaf(), which wants a
 * page-table node MCS lock this path is holding.  On exhaustion the fast path
 * declines and the VMA path allocates with reclaim, under mm->lock, where
 * sleeping is legal.
 *
 * Returns 1 if it served the fault, 0 if the caller must take the VMA path. */
static int mm_fault_from_status(task_t *t, mm_struct_t *mm,
                                uint64_t page_va, uint64_t stval)
{
    mm_cursor_t qcur;
    if (mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &qcur) != 0)
        return 0;

    uint8_t cls_byte = 0;
    int already = mm_cursor_query(&qcur, page_va, &cls_byte, NULL);
    /* A userfaultfd registration over this entry must win: the fault has to be
     * parked for the handler, not satisfied here.  The mark is per entry
     * precisely so this decision needs no VMA. */
    if (already || MM_ST_GET_CLASS(cls_byte) != MM_ST_ANON_VIRT ||
        mm_cursor_safe_test(&qcur, page_va, MM_SAFE_UFFD))
        goto decline;

    /* Round-trip the recorded prot bits back to PTE flags: they were produced by
     * mm_pt_prot_bits() from the same encoding, so the access check matches the
     * VMA path exactly. */
    int prot = 0;
    if (cls_byte & MM_ST_PROT_R) prot |= 1;
    if (cls_byte & MM_ST_PROT_W) prot |= 2;
    if (cls_byte & MM_ST_PROT_X) prot |= 4;
    /* Ask the architecture for the flag set rather than assembling PTE bits
     * here: riscv64 needs PTE_U, x86_64 additionally needs PTE_LEAF and an
     * explicit NX, and the helper encodes the W=>R dependency. */
    pte_t allow = mm_prot_to_pte_flags(prot);
    if (!mm_pte_flags_allow_access(allow))
        goto decline;

    pfn_t np = pfa_alloc_flags(0, 0);
    if (np == PFN_NONE)
        goto decline;
    if (cg_mem_charge(t->cgroup, 1) != 0) {
        frame_put(np);
        goto decline;
    }
    memset(pfn_to_virt(np), 0, PAGE_SIZE);
    if (mm_cursor_map(&qcur, page_va, pfn_to_phys(np), allow,
                      MM_ST_ANON_MAPPED) != 0) {
        cg_mem_uncharge(t->cgroup, 1);
        frame_put(np);
        goto decline;
    }

    mm_cursor_unlock(&qcur);
    mm_rss_add(mm, 1);
    a20_perf_count(A20_PERF_MM_ANON_FAULTS);
    a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
    a20_perf_count(A20_PERF_MM_FAULT_FROM_STATUS);
    /* The per-task / global soft-fault counters the VMA paths bump alongside
     * rss++; a fault path that skips them makes every reader of these, and
     * /proc's reported fault rate, wrong. */
    __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
    arch_tlb_flush_page_local(stval);
    return 1;

decline:
    mm_cursor_unlock(&qcur);
    return 0;
}
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

int handle_demand_fault_access(task_t *t, uint64_t stval,
                               enum mm_fault_access access)
{
#ifdef CONFIG_NOMMU
    return handle_demand_fault_locked(t, stval, access, 0);
#else
    /* MM_FAULT_RETRY is a benign VMA-identity change under the fault-around
     * window, so the entry point owns the retry rather than letting each
     * handler roll its own.  Re-entering re-resolves the VMA under the lock,
     * which is exactly the step the window could not do while unlocked. */
    for (int attempt = 0; attempt < MM_FAULT_RETRY_MAX; attempt++) {
        int r = handle_demand_fault_attempt(t, stval, access);
        if (r != MM_FAULT_RETRY)
            return r;
    }
    return -1;
#endif
}

#ifndef CONFIG_NOMMU
static int handle_demand_fault_attempt(task_t *t, uint64_t stval,
                                       enum mm_fault_access access)
{
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* Lock-free: the status fast path runs before mm->lock is taken. */
    if (mm_fault_from_status(t, mm, page_va, stval))
        return 0;
#endif
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


    mm_seg_t *vma = mm_seg_find(mm, page_va);
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
    /* P6: the backing object comes off the page-table path, not off the VMA.
     *
     * The segment named for this address carries everything the file-fault
     * path needs -- the open file description, sharedness, protection, the
     * object offset for this very page, and the end of the mapping -- so when
     * one covers the address it IS the authority.  `vma` is still consulted, for two reasons that are both
     * temporary and both load-bearing: it is the fallback for addresses no
     * segment covers yet, and shadow_seg_check() counts every disagreement
     * between the two resolutions.  A disagreement is a wrong-page fault if the segment
     * wins, so it is counted, not obeyed.
     *
     * Put here and not in handle_demand_fault_locked(): this dispatcher claims
     * VM_FILE first and only falls through when it declines, so instrumenting
     * the other one records nothing and looks like a passing measurement. */
    mm_seg_t *seg = mm_pt_lookup_seg(mm, page_va);
    if (seg && mm_seg_kind(seg) == MM_SEG_FILE && seg->file) {
        mm_seg_dispatch_seg++;
        if (vma)
            shadow_seg_check(mm, vma, page_va, MM_SEG_FILE, vma->backing_offset);

        uint64_t sflags = seg->vm_flags;
        int prot = 0;
        if (sflags & VM_READ)  prot |= PROT_READ;
        if (sflags & VM_WRITE) prot |= PROT_WRITE;
        if (sflags & VM_EXEC)  prot |= PROT_EXEC;
        pte_t seg_ptef = mm_prot_to_pte_flags(prot);
        if (!mm_pte_flags_allow_access(seg_ptef)) {
            mm_seg_put(seg);
            spin_unlock(&mm->lock);
            return -1;
        }
        int shared   = mm_seg_shared(seg);
        int file_pos = (int)(seg->backing_offset + (page_va - seg->start));
        vaddr_t seg_end = seg->end;
        /* Take our own reference on the open file description: the segment's
         * own reference can drop the moment mm_seg_put() does, and the fault
         * runs with mm->lock released. */
        vfile_t *seg_vf = seg->file;
        vfile_get(seg_vf);
        mm_seg_put(seg);
        /* The page-table path for this range may not have existed when this
         * fault arrived -- mmap builds no path, and the first touch is what
         * creates it.  Re-apply the mapping's segment so the NEXT fault here
         * is answerable from the page tables alone.  Without this the segment
         * table answers almost nothing: measured 6532 hits against 37626
         * misses. */
        if (vma)
            mm_mmap_seg_label(mm, vma);

        /* Writable private mappings stay on the single-page COW path.  A
         * read-only private mapping, including executable text, can share the
         * canonical page-cache frame.  mprotect(PROT_WRITE) converts the leaf
         * to COW before exposing writes, and unmap/exit drops the mapping's
         * cache pin.  This avoids allocating and copying the same rustc text
         * pages independently in every parallel compiler process. */
        int executable = (seg_ptef & PTE_X) != 0;
#ifdef CONFIG_LOONGARCH64
        /* LoongArch64 cannot yet retain private page-cache leaves safely
         * across the parallel loader/fault lifetime.  Keep private file pages
         * on the proven single-page copy path; direct executable leaves lose
         * text PTEs, while direct read-only leaves corrupt dynamic symbols in
         * librustc_driver under parallel compile load. */
        int fault_around = 0;
#else
        int fault_around = !shared && !(seg_ptef & PTE_W);
#endif
        spin_unlock(&mm->lock);
        vfile_t *vf = seg_vf;
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file(vf);
            return -1;
        }
        int r = handle_file_fault(t, page_va, file_pos, seg_end,
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
    mm_seg_put(seg);
    if (vma && (vma->vm_flags & VM_FILE) && vma->file) {
        mm_seg_dispatch_fallback++;        if (!mm_pte_flags_allow_access(vma->pte_flags)) {
            spin_unlock(&mm->lock);
            return -1;
        }
        int shared = (vma->vm_flags & VM_SHARED) != 0;
        /* Fallback: no segment covers this address yet.  This is the coverage
         * gap P6 has to close before the VMA query can go -- measured, not
         * guessed: seg_miss in the [MM-ASM] line. */
        shadow_seg_check(mm, vma, page_va, MM_SEG_FILE, vma->backing_offset);
        mm_mmap_seg_label(mm, vma);
        /* Writable private mappings stay on the single-page COW path.  A
         * read-only private mapping, including executable text, can share the
         * canonical page-cache frame.  mprotect(PROT_WRITE) converts the leaf
         * to COW before exposing writes, and unmap/exit drops the mapping's
         * cache pin.  This avoids allocating and copying the same rustc text
         * pages independently in every parallel compiler process. */
        int executable = (vma->pte_flags & PTE_X) != 0;
#if ARCH_FAULT_AROUND_UNSAFE
        /* This architecture cannot yet retain private page-cache leaves safely
         * across the parallel loader/fault lifetime.  Keep private file pages
         * on the proven single-page copy path; direct executable leaves lose
         * text PTEs, while direct read-only leaves corrupt dynamic symbols in
         * librustc_driver under parallel compile load. */
        int fault_around = 0;
#else
        int fault_around = !shared && !(vma->pte_flags & PTE_W);
#endif
        uint64_t vma_end = vma->end;
        uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
        vfile_t *vf = vma->file;
        vfile_get(vf);
        spin_unlock(&mm->lock);
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file(vf);
            return -1;
        }
        int r = handle_file_fault(t, page_va, file_pos, vma_end,
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
        uint64_t voff = vma->backing_offset + (page_va - vma->start);
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
}

#endif /* !CONFIG_NOMMU */

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
     * The PTE is not the only authority on what this address may be used
     * for: the VMA is.  A leaf can carry an execute bit the VMA never
     * granted -- a stack page whose leaf was installed from a stale flag
     * word, or a COW copy that propagated PTE_X from the parent's leaf --
     * and on every architecture whose descriptor encodes UXN/PXN
     * (aarch64's arch_pte_leaf() derives them from PTE_X) that leaf is then
     * genuinely executable at EL0.  Trusting it alone turns a jump through
     * such a leaf into a "handled" fault: the retry succeeds, the PC does
     * not advance to anything meaningful, and the same address faults again
     * immediately.  That is an unbounded silent trap loop -- a shell whose
     * stack page went executable spins on a prefetch abort at a stack
     * address until something external kills the machine, with no fault
     * report, because every round trip reported success.
     *
     * Refuse the access whenever the VMA covering the address does not grant
     * it.  The leaf may then be as wrong as it likes and the worst outcome is
     * the correct one: a clean SIGSEGV naming a mapping the process was never
     * allowed to execute.
     */
    if (allowed) {
        mm_seg_t *vma = mm_seg_find(mm, stval);
        if (vma) {
            if (access == MM_FAULT_ACCESS_EXEC &&
                !((vma->pte_flags & PTE_X) && (vma->vm_flags & VM_EXEC)))
                allowed = 0;
            else if (access == MM_FAULT_ACCESS_WRITE &&
                     !(vma->vm_flags & VM_WRITE))
                allowed = 0;
        }
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
