#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "mm/slab.h"
#include "mm/pt.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/perf.h"

/*
 * VMA list management: sorted non-overlapping mm_seg_t chain plus the backing
 * resource reference helpers used by split/merge/teardown.  Kept separate from
 * the page-table operations in mm/vm.c so the hot map/unmap/protect paths stay
 * readable.
 */

/*
 * Decide whether two adjacent VMAs may coalesce into one.  Merging is only
 * sound when the merged VMA would map exactly the same backing store over the
 * same permissions, so every case below is a correctness precondition rather
 * than a heuristic:
 *
 * - Adjacency: a->end == b->start.  The list is kept sorted, so this also
 *   asserts the caller's ordering.
 * - VM_SYSV_SHM: never merged.  SysV segments are identified by shmid in
 *   ipc/sysv_shm.c, not by file offset, so two adjacent segments are distinct
 *   objects that must stay separable for shmctl(IPC_RMID).
 * - Identical vm_flags and pte_flags: the merged PTE would otherwise have to
 *   pick one protection for the union of two different ones.
 * - VM_FILE: the same open file description AND contiguous file offset.  The
 *   offset check is what keeps a->end mapping a->backing_offset + length; without
 *   it a merged VMA would shift b's data.
 * - VM_VMO: the same vmo AND contiguous vmo_offset, for the same reason.  The
 *   second test is redundant with the vm_flags equality above but is kept
 *   explicit because the offset is a separate field.
 * - Anything else (anonymous, stack, guard) is always mergeable.
 *
 * A false negative only costs a VMA, so the checks err toward refusing.
 */
static int vma_can_merge(mm_seg_t *a, mm_seg_t *b)
{
    if (!a || !b || a->end != b->start)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_SYSV_SHM)
        return 0;
    if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_FILE) {
        if (a->file != b->file)
            return 0;
        return a->backing_offset + (a->end - a->start) == b->backing_offset;
    }
    if ((a->vm_flags | b->vm_flags) & VM_VMO) {
        if (!(a->vm_flags & VM_VMO) || !(b->vm_flags & VM_VMO))
            return 0;
        if (a->vmo != b->vmo)
            return 0;
        return a->backing_offset + (a->end - a->start) == b->backing_offset;
    }
    return 1;
}

/* Create a mapping record.  Every creator goes through here so the two fields
 * that must never start out wrong -- magic (the use-after-free check in
 * mm_seg_put) and release (what runs at the end) -- are set once, in one place,
 * rather than at each of the six allocation sites. */
mm_seg_t *mm_seg_new(void)
{
    mm_seg_t *m = (mm_seg_t *)kcalloc_atomic(1, sizeof(*m));
    if (!m)
        return NULL;
    m->magic = MM_SEG_MAGIC;
    refcount_set(&m->refcount, 1);
    /* kcalloc leaves 0, which is a real SysV id, so a mapping that never had
     * one set would claim a real shared-memory segment. */
    m->sysv_shmid = -1;
    m->release = vma_release;
    return m;
}

void vma_release_file(mm_seg_t *vma)
{
    if (vma && (vma->vm_flags & VM_FILE) && vma->file) {
        if (vma->file_vnode) {
            if (vma->vm_flags & VM_SHARED)
                vnode_shared_map_dec(vma->file_vnode);
            vnode_put(vma->file_vnode);
            vma->file_vnode = NULL;
        }
        vfs_put_file(vma->file);
        vma->file = NULL;
    }
}

void vma_release_ipc(mm_seg_t *vma)
{
    if (vma && (vma->vm_flags & VM_SYSV_SHM))
        sysv_shm_unref_attach(vma->sysv_shmid);
}

/* Drop every backing reference the record owns.  Installed as its `release`
 * callback, so this runs once when the last of the address-space list, the
 * cursors and the page-table node entries have all let go.  Idempotent: every
 * step clears the field it acted on, and a put on a record already at zero
 * cannot reach here twice. */
void vma_release(mm_seg_t *vma)
{
    vma_release_file(vma);
    vma_release_ipc(vma);
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo) {
        vmo_release(vma->vmo);
        vma->vmo = NULL;
        vma->vm_flags &= ~(uint64_t)VM_VMO;
    }
}

int vma_ref_file(mm_seg_t *vma)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || !vma->file)
        return 0;
    if (vma->file_vnode) {
        vnode_get(vma->file_vnode);
        if (vma->vm_flags & VM_SHARED)
            vnode_shared_map_inc(vma->file_vnode);
    }
    /* The forked/copied VMA owns its own vfile reference. */
    vfile_get(vma->file);
    return 0;
}

int vma_ref_fork(mm_seg_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_SYSV_SHM)) {
        r = sysv_shm_ref_attach(vma->sysv_shmid);
        if (r < 0) {
            vma_release_file(vma);
            return r;
        }
    }
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

/*
 * Retain every backing resource a copied VMA must own independently of the
 * original.  Used by the split paths (vma_split, mm_split_vma_at, munmap
 * split) after the tail VMA has been struct-copied from the head.
 */
int vma_ref_aux(mm_seg_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

/* ---- The ordered index, and the one lookup -------------------------------- */

/*
 * mm->seg_index[] is an ordered, cached copy of mm->mmap.  There was a second
 * array for this -- vma_index[] -- and it is gone: the entries of both were
 * the same pointers, so one of them was always stale the moment the other was
 * rebuilt, and mm_find_vma() and mm_seg_find() were two binary searches over
 * two arrays that had to be invalidated together or they disagreed about the
 * answer to "what covers this address".
 *
 * Drop the references the index owns.  Separate from invalidate() because
 * invalidation happens on every mapping mutation and must not touch refcounts:
 * this is the O(n) part, and paying it per mutation would put a per-mmap
 * refcount storm on the hot path.
 *
 * So invalidation keeps the stale entries and only clears the state; the
 * rebuild overwrites them, dropping what it overwrites.  That is safe only
 * because nothing reads the array while state != 1 -- the lookup below
 * rebuilds before it reads, or falls through to the list without reading it at
 * all (state 2). */

/* How many times an address space has been found over MM_SEG_INDEX_CAPACITY
 * mappings and fallen back to the list walk.  See the overflow branch in
 * mm_seg_index_rebuild() for why this is counted rather than assumed. */
uint64_t mm_seg_index_overflow;

static void mm_seg_index_rebuild(mm_struct_t *mm)
{
    /* Release the previous pass's references BEFORE writing any new entry.
     * The rebuild writes in place, so putting afterwards would put slots the
     * new pass had already overwritten -- dropping references the new entries
 * now hold, and leaking the old ones. */
    uint16_t old_count = (mm->seg_index_state == 1) ? mm->seg_index_count : 0;
    for (uint16_t i = 0; i < old_count; i++) {
        mm_seg_put(mm->seg_index[i]);
        mm->seg_index[i] = NULL;
    }

    uint16_t count = 0;
    /* The list is already sorted by start, so walking it in order produces a
     * sorted index -- no sort step. */
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (count == MM_SEG_INDEX_CAPACITY) {
            /* Over capacity.  Release what this pass took and fall back to the
             * list, which is always correct.
             *
             * Counted, because "correct" is all this path has always claimed
             * and nothing ever checked it: the software gates run processes
             * with 14 mappings against a capacity of 1024, so this branch had
             * never executed anywhere.  A zero here now means the fallback is
             * still unexercised rather than silently unexercised -- the
             * difference is whether a reader has to take the word for it. */
            mm_seg_index_overflow++;
            for (uint16_t i = 0; i < count; i++) {
                mm_seg_put(mm->seg_index[i]);
                mm->seg_index[i] = NULL;
            }
            mm->seg_index_count = 0;
            mm->seg_index_state = 2;
            return;
        }
        mm->seg_index[count++] = mm_seg_get(v);
    }
    mm->seg_index_count = count;
    mm->seg_index_state = 1;
}

void mm_seg_index_invalidate(mm_struct_t *mm)
{
    if (!mm)
        return;
    mm->seg_index_state = 0;
    mm->seg_index_count = 0;
}

void mm_seg_index_clear(mm_struct_t *mm)
{
    if (!mm)
        return;
    if (mm->seg_index_state == 1) {
        for (uint16_t i = 0; i < mm->seg_index_count; i++)
            mm_seg_put(mm->seg_index[i]);
    }
    memset(mm->seg_index, 0, sizeof(mm->seg_index));
    mm->seg_index_count = 0;
    mm->seg_index_state = 0;
}

/* What covers this address, or NULL.  The single interval lookup for the whole
 * address space: there is no second one to keep in step with it. */
mm_seg_t *mm_seg_find(mm_struct_t *mm, vaddr_t addr)
{
    if (!mm)
        return NULL;
    size_t steps = 0;
    a20_perf_count(A20_PERF_VMA_LOOKUPS);

    /* Rebuild only from state 0 (dirty).  The test used to be `!= 1`, which
     * made state 2 -- capacity overflow -- re-run the whole rebuild on EVERY
     * lookup: walk the list, take MM_SEG_INDEX_CAPACITY references, discover
     * the overflow, put them all back, and only then fall through to the list
     * walk the rebuild was a shortcut for.  So the over-capacity path paid a
     * full rebuild plus the walk it was meant to replace, per fault.
     *
     * It is also the case that most needs the memo: an address space over
     * MM_SEG_INDEX_CAPACITY mappings stays over it, so nothing about the retry
     * can change the answer.  mm_seg_index_invalidate() still forces the retry
     * (state 0), because a mutation is exactly what could bring the count back
     * under the cap -- one rebuild attempt per mapping change instead of one
     * per lookup.
     *
     * Measured on smoke-mm-seg-index-overflow, one line changed: the same
     * workload read 10407 overflow rebuilds with `!= 1` and 2791 with `== 0`.
     * The residue is the invalidate-per-mutation cost and is meant to be there
     * -- those are the rebuilds that can still change the answer. */
    if (mm->seg_index_state == 0)
        mm_seg_index_rebuild(mm);

    if (mm->seg_index_state == 1) {
        /* The last entry starting at or below `addr`; it covers `addr` only if
         * it has not ended. */
        size_t lo = 0;
        size_t hi = mm->seg_index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            steps++;
            if (mm->seg_index[mid]->start <= addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            mm_seg_t *v = mm->seg_index[lo - 1];
            steps++;
            if (addr < v->end) {
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
        return NULL;
    }

    /* Very unusual address spaces with more than MM_SEG_INDEX_CAPACITY mappings
     * retain the proven linked-list behavior instead of allocating while
     * mm->lock is held. */
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        steps++;
        if (steps > 100000u) {
            kerr("[VMAWALK] CYCLE pid? mm=%p addr=0x%lx steps=%u head=%p\n",
                 (void *)mm, (unsigned long)addr, (unsigned)steps,
                 (void *)mm->mmap);
            panic("mm_seg_find: mapping list cycle");
        }
        if (addr < v->end && addr >= v->start) {
            a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
            return v;
        }
        if (v->start > addr)
            break;
    }
    a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
    return NULL;
}

/*
 * Return a temporary reference when a file VMA leaf is backed directly by
 * the canonical page-cache page.  A private file mapping can contain both
 * kinds of leaves: clean read-only cache pages and anonymous pages created by
 * COW.  Comparing the PFN, rather than classifying the whole VMA, keeps
 * fork/mprotect/mremap/teardown correct after only some pages were copied.
 */
page_cache_page_t *mm_file_cache_mapping_get(mm_seg_t *vma, vaddr_t va,
                                              pfn_t pfn)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || !vma->file_vnode ||
        va < vma->start || va >= vma->end || !pfn_valid(pfn))
        return NULL;
    uint64_t index = vma->backing_offset + (va - vma->start);
    index /= PAGE_SIZE;
    page_cache_page_t *page = page_cache_get(vma->file_vnode, index, 0);
    if (page && page_cache_pfn(page) == pfn)
        return page;
    if (page)
        page_cache_put(page);
    return NULL;
}

vaddr_t mm_find_gap(mm_struct_t *mm, vaddr_t hint, size_t len) {
    vaddr_t prev_end = hint;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      mm_seg_t *ignore) {
    vaddr_t end = start + len;
    if (end < start) return 1;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v == ignore) continue;
        if (v->start < end && v->end > start)
            return 1;
        if (v->start >= end) break;
    }
    return 0;
}

/*
 * MM_AS_VMA_REFCOUNT -- mapping lifetime.
 *
 * mm_seg_t carries a reference count so a page fault can read a mapping's
 * fields with mm->lock released; that is what stops one address-space lock
 * from serialising every fault in the process.  Three holders share the count:
 * the address-space list, the ordered index over it, and the page-table node
 * entries.  Unlinking drops the list's; the LAST holder schedules the free.
 *
 * The record is also one object, so its release is now the whole mapping's
 * release -- vma_release_file() closes the fd, which is blocking I/O
 * (vfs_close -> page cache writeback).  The last holder therefore does not run
 * it: it pushes onto the deferred list under vma_ref_lock, taking a reference
 * for the queue, and an existing flush point runs the release and drops the
 * queue's reference.  Without the queue's own reference, a node-entry or index
 * put reaching zero later would free a record the queue was still holding.
 *
 * That lock is deliberately NOT mm->lock: a lock-free fault must be able to
 * defer its free without re-acquiring the lock it just escaped, and holding
 * mm->lock across vma_release() is what the deferred list exists to avoid.
 */
void vma_get(mm_seg_t *vma)
{
    if (vma)
        refcount_inc(&vma->refcount);
}

void vma_put(mm_struct_t *mm, mm_seg_t *vma)
{
    if (!mm || !vma) return;
    if (!refcount_dec_and_test(&vma->refcount))
        return;

    /* The queue takes a reference of its own.  It has to: this record is one
     * object now, so the page-table node entries and the address-space index
     * hold references to the same thing the list does, and any of them can be
     * the one that reaches zero later.  Without a reference here, that later
     * put would free the record while this queue was still holding its
     * address, and the flusher would then read freed memory to decide whether
     * to free it again. */
    mm_seg_get(vma);

    uint64_t flags = spin_lock_irqsave(&mm->vma_ref_lock);
    vma->deferred_next = mm->deferred_vma;
    mm->deferred_vma = vma;
    spin_unlock_irqrestore(&mm->vma_ref_lock, flags);
}

// Unlink-time release: drops the address-space list's reference.  Callers
// keep their existing shape -- they unlink under mm->lock and drop it before
// flushing, so the observable ordering is unchanged.
void mm_vma_defer(mm_struct_t *mm, mm_seg_t *vma)
{
    vma_put(mm, vma);
}

void mm_vma_flush_deferred(mm_struct_t *mm)
{
    if (!mm) return;

    /*
     * Detach the whole list under vma_ref_lock so two threads sharing an mm
     * cannot both observe and free the same chain.  The detached list is
     * private to this flusher; potentially sleeping vma_release() work stays
     * outside the spinlock.
     */
    uint64_t flags = spin_lock_irqsave(&mm->vma_ref_lock);
    mm_seg_t *v = mm->deferred_vma;
    mm->deferred_vma = NULL;
    spin_unlock_irqrestore(&mm->vma_ref_lock, flags);

    while (v) {
        mm_seg_t *next = v->deferred_next;
        /* The release runs here and nowhere else.  It can block (vfs_close
         * reaches the page cache), and the callers of mm_seg_put() are page
         * fault and index paths that hold mm->lock or run with no lock at all.
         * `released` tells mm_seg_put() that the work is already done when it
         * is this queue's own put that finally frees the record. */
        v->released = 1;
        vma_release(v);
        mm_seg_put(v);
        v = next;
    }
}

void mm_insert_vma(mm_struct_t *mm, mm_seg_t *newv) {
    mm_seg_index_invalidate(mm);
    mm_seg_t **pp = &mm->mmap;
    mm_seg_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;

    /* Whichever mapping survives the merges below; see the re-annotate below. */
    mm_seg_t *survivor = newv;

    if (vma_can_merge(newv, newv->next)) {
        mm_seg_t *nxt = newv->next;
        /* Stop naming nxt's extent before the survivor grows over it.  These
         * two merges were the only mutators that dropped a mapping from the
         * list without retiring its annotation first: every unmap, split and
         * protect path does, and without it the shared entry keeps naming the
         * absorbed record for good.  A node entry is much coarser than a
         * mapping (2 MiB on x86_64), so ld.so mapping consecutive segments of
         * one shared object routinely leaves one entry naming both the survivor
         * and the mapping it just absorbed -- and a fault into the absorbed
         * half then resolved two live records and declined to guess.
         *
         * Retire BEFORE the extent grows, so the range passed is nxt's own and
         * not the survivor's. */
        mm_mmap_seg_retire(mm, nxt, nxt->start, nxt->end);
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        mm_vma_defer(mm, nxt);
    }
    if (vma_can_merge(newv->prev, newv)) {
        mm_seg_t *prv = newv->prev;
        mm_mmap_seg_retire(mm, newv, newv->start, newv->end);
        prv->end = newv->end;
        prv->next = newv->next;
        if (newv->next) newv->next->prev = prv;
        mm_vma_defer(mm, newv);
        survivor = prv;
    }

    /* Build this mapping's segment HERE rather than at each of the nine
     * mm_insert_vma() call sites.  Six of them created anonymous mappings and
     * never annotated: brk growth (munmap.c), the ELF stack and bss (elf.c),
     * SysV shm, the two framebuffer paths and io_uring.  Measured on the gate:
     * 8 of the 14 VMAs in the audited process had no segment at all, so an
     * ordered index over segments would have been an index over the minority
     * of mappings -- exactly the hole that made this step worth doing first.
     *
     * It has to run after the merges above, because the survivor is not always
     * newv: a merge with the previous mapping absorbs newv into prv, and then
     * it is prv's extent that grew.  Re-annotating newv there would have built
     * a segment on a mapping that is already deferred, and left the real
     * survivor describing its pre-merge extent. */
    if (survivor)
        mm_mmap_seg_annotate(mm, survivor, survivor->start, survivor->end);
}

int mm_split_vma_at(mm_struct_t *mm, vaddr_t addr) {
    mm_seg_t *v = mm_seg_find(mm, addr);
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

    mm_seg_t *tail = mm_seg_new();
    if (!tail)
        return -ENOMEM;

    *tail = *v;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* Stop naming the pre-split extent before either half is narrowed: the
     * cut-away tail is exactly the range being detached, and an entry still
     * naming it would resolve for the next mapping to land there. */
    mm_mmap_seg_retire(mm, v, v->start, v->end);
#endif
    refcount_set(&tail->refcount, 1);   /* the copy brought its count along */
    tail->start = addr;
    tail->backing_offset += addr - v->start;
    int fr = vma_ref_aux(tail);
    if (fr < 0) {
        kfree(tail);
        return fr;
    }
    mm_seg_index_invalidate(mm);
    tail->prev = v;
    tail->next = v->next;
    if (tail->next)
        tail->next->prev = tail;

    v->end = addr;
    v->next = tail;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* Name the head first, then the tail, for the same ordering reason the
     * munmap middle cut uses: the two walks must not both claim the node entry
     * that straddles the boundary. */
    mm_mmap_seg_annotate(mm, v, v->start, v->end);
    mm_mmap_seg_annotate(mm, tail, tail->start, tail->end);
#endif
    return 0;
}

/*
 * Split @vma at @split and return the new tail.
 *
 * MM_SEG_INDEX_MUTATION: this mutates the address space's record list, so it must
 * take @mm and drop mm's cached lookup index.  It used to take only the VMA and
 * leave the index alone, and mprotect -- its only caller -- was the one mutator
 * in the tree that did not invalidate.  The consequence was not a stale VMA but
 * a *misaligned* one: the index array is a snapshot of the list's pointers, so
 * inserting the tail leaves every later slot one position behind.  The array is
 * still sorted by start, so the binary search inside mm_seg_find() converges
 * without complaint -- onto a VMA that ends before the address, or onto none at
 * all.  A lookup then reports "unmapped" for a range that is mapped, and the
 * callers act on that: mm_split_vma_at() declines to split, mprotect() returns
 * success having changed nothing, and handle_demand_fault_locked() answers -1
 * for a store into a perfectly good anonymous page.  Under a single CPU the
 * window is too narrow to hit; with threads sharing an address space it is the
 * difference between a program that runs and one that dies of SIGSEGV.
 *
 * Taking @mm here rather than asking the caller to remember is deliberate: this
 * is the only VMA-splitting helper that cannot invalidate on its own, and the
 * signature is what makes the obligation impossible to forget.
 */
mm_seg_t *vma_split(mm_struct_t *mm, mm_seg_t *vma, vaddr_t split) {
    if (!vma) return NULL;
    if (split <= vma->start || split >= vma->end) return vma;

    mm_seg_t *tail = mm_seg_new();
    if (!tail) return NULL;

    *tail = *vma;
    refcount_set(&tail->refcount, 1);   /* the copy brought its count along */
    tail->start = split;
    tail->backing_offset += split - vma->start;
    if (vma_ref_aux(tail) < 0) {
        kfree(tail);
        return NULL;
    }
    mm_seg_index_invalidate(mm);
    tail->prev = vma;
    tail->next = vma->next;
    if (tail->next) tail->next->prev = tail;

    vma->end = split;
    vma->next = tail;
    return tail;
}

mm_seg_t *vma_try_merge(mm_struct_t *mm, mm_seg_t *vma) {
    if (!vma) return NULL;

    mm_seg_index_invalidate(mm);

    if (vma_can_merge(vma->prev, vma)) {
        mm_seg_t *prev = vma->prev;
        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) vma->next->prev = prev;
        mm_vma_defer(mm, vma);
        vma = prev;
    }

    if (vma_can_merge(vma, vma->next)) {
        mm_seg_t *next = vma->next;
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) next->next->prev = vma;
        mm_vma_defer(mm, next);
    }
    return vma;
}

void free_vma_pages(mm_struct_t *mm, mm_seg_t *vma)
{
#ifdef CONFIG_NOMMU
    (void)mm;
    if (vma->nommu_alloc) {
        kfree(vma->nommu_alloc);
        vma->nommu_alloc = NULL;
    }
    /* In NOMMU, we now track kmalloc allocations in mm->nommu_allocs.
     * We don't free vma->start directly here to avoid double-frees and freeing
     * invalid pointers if the VMA was split or modified by mprotect/munmap. */
#else
    if (!mm->pgdir) return;
    int shared_file = (vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    for (uint64_t va = vma->start; va < vma->end; ) {
        /* Check whether the current mapping is a huge page that straddles a
         * VMA boundary; if so, demote it to base pages and release them
         * one at a time. */
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && level > 0 &&
            (base < vma->start || base + size > vma->end)) {
            mm_demote_huge_page(mm, va);
            continue;
        }

        if (pte && (*pte & PTE_V) && shared_file && vma->file_vnode &&
            (*pte & PTE_D)) {
            uint64_t idx = vma->backing_offset + (va - vma->start);
            idx /= PAGE_SIZE;
            page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
            if (pcp) {
                page_cache_mark_dirty(pcp);
                page_cache_put(pcp);
            }
        }

        page_cache_page_t *mapped_page = NULL;
        if (pte && (*pte & PTE_V)) {
            pfn_t mapped_pfn = phys_to_pfn(arch_pte_addr(*pte));
            mapped_page = mm_file_cache_mapping_get(vma, va, mapped_pfn);
        }

        paddr_t pa = 0;
        base = 0;
        size = 0;
        if (pt_unmap_leaf(mm, va, &pa, &base, &size, NULL) == 0) {
            if (pa) {
                pfn_t pfn = phys_to_pfn(pa);
                if (mapped_page) {
                    /* Temporary lookup plus the PTE's retained cache pin. */
                    page_cache_put(mapped_page);
                    page_cache_put(mapped_page);
                } else if (!(vma->vm_flags & (VM_PFNMAP | VM_VMO))) {
                    /* VMO frames are owned by the VMO (see vmo_get_page);
                     * unmapping a PTE never releases them — same rule as
                     * mm_munmap_locked()/madvise.  Putting them here freed
                     * live VMO pages while other mappers (and vmo->pages[])
                     * still referenced them. */
                    frame_put(pfn);
                }
            }
            va = base + size;
        } else {
            if (mapped_page)
                page_cache_put(mapped_page);
            va += PAGE_SIZE;
        }
    }
#endif
}
