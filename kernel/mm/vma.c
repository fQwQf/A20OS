#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "mm/vmar.h"
#include "mm/slab.h"
#include "mm/pt.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"

/* munmap_txn.rs: release transaction for run-batched final-destroy leaves. */
extern void a20_vma_free_run_release_rust(mm_seg_t *vma, uintptr_t start_va,
                                          const pfn_t *pfns, size_t count,
                                          int no_frame_ref);
#include "core/perf.h"

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
static int mm_dontunmap_split_failure_consumed;
#endif

/* The list/backing pointers stay C-owned; Rust consumes only stable snapshots
 * while the caller holds mm->lock. Keep these adapters narrow and side-effect
 * free so interval and coalescing policy has one implementation. */
struct a20_vma_region_snapshot {
    uintptr_t start;
    uintptr_t end;
    const void *next;
};
struct a20_vma_merge_snapshot {
    uintptr_t start;
    uintptr_t end;
    uint64_t backing_offset;
    uint64_t vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap;
    uint32_t reserved;
    const void *vmar_owner;
    const void *file;
    const void *vmo;
    const void *prev;
    const void *next;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_vma_region_snapshot) == 24,
               "Rust/C VMA region snapshot ABI");
_Static_assert(sizeof(struct a20_vma_merge_snapshot) == 88,
               "Rust/C VMA merge snapshot ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_vma_region_snapshot) == 12,
               "Rust/C VMA region snapshot ABI");
_Static_assert(sizeof(struct a20_vma_merge_snapshot) == 56,
               "Rust/C VMA merge snapshot ABI");
#endif
extern uintptr_t a20_vma_find_gap_rust(const void *node, uintptr_t hint,
                                        size_t length);
extern int a20_vma_range_overlaps_rust(const void *node, uintptr_t start,
                                        size_t length, const void *ignore);
extern int a20_vma_split_transaction_rust(void *mm, void *head,
                                           uintptr_t split, unsigned int test_split,
                                           void **tail_out);
extern void *a20_vma_merge_transaction_rust(void *mm, void *node,
                                             int left_first);

int a20_vma_region_read(const void *node,
                        struct a20_vma_region_snapshot *out)
{
    const mm_seg_t *v = node;
    if (!v || !out)
        return -1;
    out->start = v->start;
    out->end = v->end;
    out->next = v->next;
    return 0;
}

int a20_vma_merge_snapshot(const void *node,
                           struct a20_vma_merge_snapshot *out)
{
    const mm_seg_t *v = node;
    if (!v || !out)
        return -1;
    out->start = v->start;
    out->end = v->end;
    out->backing_offset = v->backing_offset;
    out->vm_flags = v->vm_flags;
    out->pte_flags = v->pte_flags;
    out->vmar_cap = v->vmar_cap;
    out->reserved = 0;
    out->vmar_owner = v->vmar_owner;
    out->file = v->file;
    out->vmo = v->vmo;
    out->prev = v->prev;
    out->next = v->next;
    return 0;
}

void a20_vma_merge_commit(void *mm_ptr, void *survivor_ptr,
                          void *absorbed_ptr)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *survivor = survivor_ptr;
    mm_seg_t *absorbed = absorbed_ptr;
    if (!mm || !survivor || !absorbed)
        return;
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    mm_mmap_seg_retire(mm, absorbed, absorbed->start, absorbed->end);
#endif
    mm_seg_index_invalidate(mm);
    survivor->end = absorbed->end;
    survivor->next = absorbed->next;
    if (absorbed->next)
        absorbed->next->prev = survivor;
    mm_vma_defer(mm, absorbed);
}

/* Split transaction primitives. Rust decides and orders the phases; these
 * callbacks perform only the narrow C operations that know the mm_seg layout,
 * backing-reference helpers, and architecture-specific segment annotations. */
void *a20_vma_split_alloc(unsigned int test_split)
{
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    int expected_unconsumed = 0;
    if (test_split == CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT &&
        __atomic_compare_exchange_n(&mm_dontunmap_split_failure_consumed,
                                    &expected_unconsumed, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        klog_write("MM_DONTUNMAP_TEST: injected-vma-allocation-failure split=%u\n",
                   test_split);
        return NULL;
    }
#else
    (void)test_split;
#endif
    return mm_seg_new();
}

int a20_vma_split_clone(void *head_ptr, void *tail_ptr, uintptr_t split)
{
    mm_seg_t *head = head_ptr;
    mm_seg_t *tail = tail_ptr;
    if (!head || !tail || split <= head->start || split >= head->end)
        return -EINVAL;
    *tail = *head;
    refcount_set(&tail->refcount, 1);
    tail->start = split;
    tail->backing_offset += split - head->start;
    return 0;
}

int a20_vma_split_ref(void *tail_ptr, uint32_t kind)
{
    mm_seg_t *tail = tail_ptr;
    if (!tail)
        return -EINVAL;
    switch (kind) {
    case 1:
        if (!(tail->vm_flags & VM_FILE) || !tail->file)
            return 0;
        return vma_ref_file(tail) < 0 ? -ENOMEM : 1;
    case 2:
        if (!(tail->vm_flags & VM_VMO) || !tail->vmo)
            return 0;
        vmo_ref(tail->vmo);
        return 1;
    case 3:
        if (!tail->vmar_owner)
            return 0;
        vmar_acquire(tail->vmar_owner);
        return 1;
    default:
        return -EINVAL;
    }
}

void a20_vma_split_unref(void *tail_ptr, uint32_t kind)
{
    mm_seg_t *tail = tail_ptr;
    if (!tail)
        return;
    switch (kind) {
    case 1:
        vma_release_file(tail);
        break;
    case 2:
        if (tail->vmo)
            vmo_release(tail->vmo);
        break;
    case 3:
        if (tail->vmar_owner)
            vmar_release(tail->vmar_owner);
        break;
    }
}

void a20_vma_split_free(void *tail_ptr)
{
    if (tail_ptr)
        kfree(tail_ptr);
}

void a20_vma_split_retire(void *mm_ptr, void *head_ptr)
{
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *head = head_ptr;
    mm_mmap_seg_retire(mm, head, head->start, head->end);
#else
    (void)mm_ptr;
    (void)head_ptr;
#endif
}

void a20_vma_split_publish(void *mm_ptr, void *head_ptr, void *tail_ptr,
                           uintptr_t split)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *head = head_ptr;
    mm_seg_t *tail = tail_ptr;
    mm_seg_index_invalidate(mm);
    tail->prev = head;
    tail->next = head->next;
    if (tail->next)
        tail->next->prev = tail;
    head->end = split;
    head->next = tail;
}

void a20_vma_split_annotate(void *mm_ptr, void *head_ptr, void *tail_ptr)
{
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *head = head_ptr;
    mm_seg_t *tail = tail_ptr;
    mm_mmap_seg_annotate(mm, head, head->start, head->end);
    mm_mmap_seg_annotate(mm, tail, tail->start, tail->end);
#else
    (void)mm_ptr;
    (void)head_ptr;
    (void)tail_ptr;
#endif
}

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
    if (vma && vma->vmar_owner) {
        vmar_release(vma->vmar_owner);
        vma->vmar_owner = NULL;
    }
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
    if (vma && vma->vmar_owner)
        vmar_acquire(vma->vmar_owner);
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
    if (vma && vma->vmar_owner)
        vmar_acquire(vma->vmar_owner);
    return 0;
}

/* ---- The ordered index, and the one lookup -------------------------------- */

/*
 * mm->seg_index (embedded or dynamic) is an ordered, cached copy of mm->mmap.  There was a second
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
 * So invalidation keeps the stale entries, keeps the count that says how many
 * of them are owned, and only clears the state; the rebuild releases the old
 * pass and writes the new one in the same walk.  That ordering is safe because
 * nothing reads the array while state != 1 -- the lookup below rebuilds before
 * it reads, or falls through to the list without reading it at all (state 2). */

/* How many rebuilds found more mappings than the embedded index can hold.
 * The index grows when allocation succeeds; this remains the proc-visible
 * capacity-exceeded diagnostic. */
uint64_t mm_seg_index_overflow;

static mm_seg_t **mm_seg_index_slots(mm_struct_t *mm)
{
    return mm->seg_index_large ? mm->seg_index_large : mm->seg_index;
}

static size_t mm_seg_index_active_capacity(mm_struct_t *mm)
{
    return mm->seg_index_large ? mm->seg_index_capacity
                               : MM_SEG_INDEX_CAPACITY;
}

static size_t mm_seg_list_count(mm_struct_t *mm)
{
    size_t count = 0;
    mm_seg_t *slow = mm->mmap;
    mm_seg_t *fast = mm->mmap;

    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (count == (size_t)-1)
            panic("mm_seg_index_rebuild: mapping count overflow");
        count++;

        slow = slow ? slow->next : NULL;
        fast = (fast && fast->next) ? fast->next->next : NULL;
        if (slow && slow == fast)
            panic("mm_seg_index_rebuild: mapping list cycle");
    }
    return count;
}

static void mm_seg_index_rebuild(mm_struct_t *mm)
{
    /* Invalidation preserves seg_index_count because these slots still own
     * references. Put them before reusing the array; state 2 has already
     * cleared its slots and count. */
    size_t old_count = mm->seg_index_count;
    mm_seg_t **slots = mm_seg_index_slots(mm);
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    for (size_t i = 0; i < old_count; i++) {
        vma_put(mm, slots[i]);
        slots[i] = NULL;
    }
    mm->seg_index_count = 0;

    size_t total = mm_seg_list_count(mm);
    size_t capacity = mm_seg_index_active_capacity(mm);
    if (total > MM_SEG_INDEX_CAPACITY)
        mm_seg_index_overflow++;
    if (total > capacity) {
        size_t grown = capacity;
        while (grown < total) {
            if (grown > (size_t)-1 / 2) {
                grown = total;
                break;
            }
            grown *= 2;
        }
        if (grown > (size_t)-1 / sizeof(*slots)) {
            mm->seg_index_state = 2;
            return;
        }
        if (!mm->seg_index_large || grown > mm->seg_index_capacity) {
            mm_seg_t **replacement = kcalloc_atomic(grown, sizeof(*replacement));
            if (!replacement) {
                /* Allocation is nonblocking because callers may hold mm->lock.
                 * Keep any empty old buffer for a later invalidation/retry. */
                mm->seg_index_state = 2;
                return;
            }
            kfree(mm->seg_index_large);
            mm->seg_index_large = replacement;
            mm->seg_index_capacity = grown;
            slots = replacement;
            capacity = grown;
        } else {
            capacity = mm->seg_index_capacity;
            slots = mm->seg_index_large;
        }
    }

    /* The list is already sorted by start, so walking it in order produces a
     * complete sorted index. Each entry owns a reference, so lookup never
     * follows an unpinned list link. */
    size_t count = 0;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (count >= capacity)
            panic("mm_seg_index_rebuild: list changed during rebuild");
        slots[count++] = mm_seg_get(v);
    }
    mm->seg_index_count = count;
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    mm->seg_index_state = 1;
}

void mm_seg_index_invalidate(mm_struct_t *mm)
{
    if (!mm)
        return;
    /* seg_index_count deliberately survives: the slots below still hold owned
     * references, and the rebuild is the pass that puts them.  Zeroing the
     * count here is what orphaned them -- see mm_seg_index_rebuild(). */
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
    mm->seg_index_state = 0;
}

void mm_seg_index_clear(mm_struct_t *mm)
{
    if (!mm)
        return;
    /* Keyed on the count, not on state == 1: a dirty index (state 0) still
     * owns every reference the last valid pass took. */
    mm_seg_t **slots = mm_seg_index_slots(mm);
    for (size_t i = 0; i < mm->seg_index_count; i++) {
        vma_put(mm, slots[i]);
        slots[i] = NULL;
    }
    memset(mm->seg_index, 0, sizeof(mm->seg_index));
    mm->seg_index_count = 0;
    kfree(mm->seg_index_large);
    mm->seg_index_large = NULL;
    mm->seg_index_capacity = 0;
    __atomic_store_n(&mm->seg_index_hint, 0, __ATOMIC_RELAXED);
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

    /* Rebuild only from state 0 (dirty). Allocation failure is memoized in
     * state 2 until a mapping mutation invalidates the index. Successful
     * over-capacity rebuilds grow the owned array rather than scan an unpinned
     * list suffix. */
    if (mm->seg_index_state == 0)
        mm_seg_index_rebuild(mm);

    if (mm->seg_index_state == 1) {
        /* The owned index already pins the record. A hint adds no lifetime
         * dependency: every hit still checks the complete covering interval. */
        mm_seg_t **slots = mm_seg_index_slots(mm);
        size_t hint = __atomic_load_n(&mm->seg_index_hint, __ATOMIC_RELAXED);
        if (hint < mm->seg_index_count) {
            steps++;
            mm_seg_t *v = slots[hint];
            if (v->start <= addr && addr < v->end) {
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        /* The last entry starting at or below `addr`; it covers `addr` only if
         * it has not ended. */
        size_t lo = 0;
        size_t hi = mm->seg_index_count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            steps++;
            if (slots[mid]->start <= addr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo > 0) {
            mm_seg_t *v = slots[lo - 1];
            steps++;
            if (addr < v->end) {
                __atomic_store_n(&mm->seg_index_hint, lo - 1,
                                 __ATOMIC_RELAXED);
                a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
                return v;
            }
        }
        a20_perf_add(A20_PERF_VMA_LOOKUP_STEPS, steps);
        return NULL;
    }

    /* Nonblocking allocation failure is memoized until the next list mutation.
     * Preserve the original linear fallback rather than dereference a partial
     * index. */
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
    return (vaddr_t)a20_vma_find_gap_rust(mm ? mm->mmap : NULL, hint, len);
}

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      mm_seg_t *ignore) {
    return a20_vma_range_overlaps_rust(mm ? mm->mmap : NULL, start, len,
                                       ignore);
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

/*
 * The queue for records whose last reference drops somewhere mm->deferred_vma
 * cannot be reached: mm_seg_put() from a page fault or an index rebuild that
 * still holds mm->lock, from inside an mcs_lock section, or from an interrupt.
 * Those paths may not run release code -- it closes the backing file, which
 * blocks on I/O, and parking the task from inside a spinlock is exactly what
 * the context_switch_locked preempt_count check rejects -- so the put parks
 * the record here instead and hands over the reference it just dropped.
 *
 * Global rather than per-mm because by construction no caller can still name
 * the mm the record belonged to; vma_release() reads only the record itself,
 * so any flush point will do.  mm_vma_flush_deferred() runs on every munmap,
 * mmap teardown and exit, which is what keeps the queue bounded.
 */
static mm_seg_t *g_seg_release_pending;
static spinlock_t g_seg_release_lock = SPINLOCK_INIT;

void mm_seg_release_defer(mm_seg_t *s)
{
    if (!s) return;
    /* Take over the reference mm_seg_put() just dropped, so the record is
     * never observed at zero: the flusher drops this one after the release. */
    mm_seg_get(s);

    uint64_t flags = spin_lock_irqsave(&g_seg_release_lock);
    s->deferred_next = g_seg_release_pending;
    g_seg_release_pending = s;
    spin_unlock_irqrestore(&g_seg_release_lock, flags);
}

static void mm_seg_release_drain(void)
{
    uint64_t flags = spin_lock_irqsave(&g_seg_release_lock);
    mm_seg_t *v = g_seg_release_pending;
    g_seg_release_pending = NULL;
    spin_unlock_irqrestore(&g_seg_release_lock, flags);

    while (v) {
        mm_seg_t *next = v->deferred_next;
        v->deferred_next = NULL;
        v->released = 1;
        vma_release(v);
        mm_seg_put(v);
        v = next;
    }
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

    /* Same contract, for the records no per-mm queue could claim. */
    mm_seg_release_drain();
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

    survivor = a20_vma_merge_transaction_rust(mm, newv, 0);

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

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
#if CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT == 0
#error "CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT=0 matches ordinary VMA splits; use 99 for trace-only"
#endif
#endif

static int mm_split_vma_at_impl(mm_struct_t *mm, vaddr_t addr,
                                unsigned int test_split)
{
    mm_seg_t *v = mm_seg_find(mm, addr);
#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    int would_allocate = v && addr > v->start && addr < v->end;
    if (test_split)
        klog_write("MM_DONTUNMAP_TEST: split-boundary call=%u vma_start=%p vma_end=%p addr=%p would_allocate=%u\n",
                   test_split,
                   v ? (void *)(uintptr_t)v->start : NULL,
                   v ? (void *)(uintptr_t)v->end : NULL,
                   (void *)(uintptr_t)addr, (unsigned)would_allocate);
#endif
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
    /* Deterministic fault injection at the real VMA-tail allocation site.
     * Return the same -ENOMEM that mm_seg_new() would return if its allocator
     * failed, but only after proving this call would actually split a live VMA. */
#else
    (void)test_split;
#endif

    void *tail = NULL;
    return a20_vma_split_transaction_rust(mm, v, addr, test_split, &tail);
}

int mm_split_vma_at(mm_struct_t *mm, vaddr_t addr)
{
    return mm_split_vma_at_impl(mm, addr, 0);
}

#ifdef CONFIG_MM_DONTUNMAP_TEST_FAIL_SPLIT
int mm_split_vma_at_dontunmap_test(mm_struct_t *mm, vaddr_t addr,
                                   unsigned int split_call)
{
    return mm_split_vma_at_impl(mm, addr, split_call);
}
#endif

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

    void *tail = NULL;
    if (a20_vma_split_transaction_rust(mm, vma, split, 0, &tail) != 0)
        return NULL;
    return tail;
}

mm_seg_t *vma_try_merge(mm_struct_t *mm, mm_seg_t *vma) {
    if (!vma) return NULL;

    return a20_vma_merge_transaction_rust(mm, vma, 1);
}

/* Sequential teardown batch: leaves cleared per pt_unmap_leaf_run() call.
 * Bounded to one L0 table (512) so the run never crosses a table; 64 keeps
 * the on-stack pa array small while still amortising the walk, node lock and
 * empty-table cascade over a full batch. */
#define A20_VMA_FREE_RUN_PAGES 64

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
    /* Runs need the per-page release to be expressible from the cleared
     * leaf's physical address alone: no shared-file dirty marking (that path
     * reads PTE_D before the unmap) and no PFNMAP/VMO frames (never released
     * here).  Everything else -- anon, anon-shared and private file pages --
     * releases exactly like the scalar body. */
    int runs_ok = !(vma->vm_flags & (VM_SHARED | VM_PFNMAP | VM_VMO |
                                     VM_SYSV_SHM));
    paddr_t run_pa[A20_VMA_FREE_RUN_PAGES];
    for (uint64_t va = vma->start; va < vma->end; ) {
        if (runs_ok) {
            /* The run must never cross the VMA boundary: pages past vma->end
             * belong to another mapping and their pins are not ours. */
            uint64_t span = (vma->end - va) / PAGE_SIZE;
            int max = span < A20_VMA_FREE_RUN_PAGES ? (int)span
                                                    : A20_VMA_FREE_RUN_PAGES;
            int n = max > 0 ? pt_unmap_leaf_run(mm, va, max, run_pa) : 0;
            if (n > 0) {
                /* Reference release is a transaction in Rust (see
                 * munmap_txn.rs): per-page PFN classification decides cache
                 * pin versus frame reference exactly once per leaf. */
                pfn_t run_pfn[A20_VMA_FREE_RUN_PAGES];
                for (int i = 0; i < n; i++)
                    run_pfn[i] = phys_to_pfn(run_pa[i]);
                int no_frame_ref =
                    (vma->vm_flags & (VM_PFNMAP | VM_VMO)) != 0;
                a20_vma_free_run_release_rust(vma, va, run_pfn, n,
                                              no_frame_ref);
                va += (uint64_t)n * PAGE_SIZE;
                continue;
            }
        }
        /* Check whether the current mapping is a huge page that straddles a
         * VMA boundary; if so, demote it to base pages and release them
         * one at a time. */
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (!pte) {
            /* An absent parent entry has no resident or swapped leaves to
             * release.  The lookup reports its whole subtree, including a
             * partial subtree at either VMA boundary.  Swap entries are
             * returned as leaves and still go through pt_unmap_leaf(). */
            uint64_t next = (uint64_t)base + size;
            va = size && next > va ? next : va + PAGE_SIZE;
            continue;
        }
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
