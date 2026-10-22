#include "fs/page_cache.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/string.h"
#include "core/lock_counters.h"

/*
 * Page-cache state transactions. Rust owns miss publication and duplicate
 * resolution, reference/pin cleanup, eviction and reclaim outcomes, dirty-
 * generation confirmation, writeback success/failure transitions, and
 * invalidate/truncate decisions. C keeps the simple warm-hit path, byte-copy
 * loops and read-ahead arithmetic, plus locks, frame allocation, vnode refs,
 * and filesystem I/O callbacks. The historical C state engine is retained in
 * host differential tests; production uses the Rust transaction path.
 */
extern unsigned a20_pagecache_hash_key(vnode_t *vn, uint64_t index);
extern void a20_pagecache_lru_remove(page_cache_page_t *page);
extern void a20_pagecache_lru_insert_front(page_cache_page_t *head, page_cache_page_t *page);
extern void a20_pagecache_lru_insert_tail(page_cache_page_t *tail, page_cache_page_t *page);
extern void a20_pagecache_free_insert(page_cache_page_t **free_head, page_cache_page_t *page);
extern page_cache_page_t *a20_pagecache_free_take(page_cache_page_t **free_head,
                                                  page_cache_page_t *lru_head);
extern void a20_pagecache_hash_insert(page_cache_page_t **hash, page_cache_page_t *page);
extern void a20_pagecache_hash_remove(page_cache_page_t **hash, page_cache_page_t *page,
                                      void (*account)(size_t));
extern page_cache_page_t *a20_pagecache_find(page_cache_page_t **hash, vnode_t *vn,
                                             uint64_t index, void (*account)(size_t));
extern void a20_pagecache_mapping_insert(page_cache_page_t *page);
extern void a20_pagecache_mapping_remove(page_cache_page_t *page);
extern void a20_pagecache_dirty_insert(page_cache_page_t *page,
                                       page_cache_page_t **global_dirty_head,
                                       size_t *global_dirty_count);
extern void a20_pagecache_dirty_remove(page_cache_page_t *page,
                                       page_cache_page_t **global_dirty_head,
                                       size_t *global_dirty_count);
extern vnode_t *a20_pagecache_detach(page_cache_page_t *page,
                                     page_cache_page_t **global_dirty_head,
                                     size_t *global_dirty_count,
                                     page_cache_page_t **hash, size_t *valid_pages,
                                     void (*account)(size_t));
extern void a20_pagecache_pin(page_cache_page_t *page, size_t *bucket_pinned);
extern void a20_pagecache_account_pin(page_cache_page_t *page, size_t *bucket_pinned);
extern int a20_pagecache_put(page_cache_page_t *page, size_t *bucket_pinned);
extern size_t a20_pagecache_pinned_total(const size_t *bucket_pinned);
extern page_cache_page_t *a20_pagecache_get_transaction(
    void *, vnode_t *, uint64_t, int, page_cache_page_t **,
    page_cache_page_t *, page_cache_page_t *, page_cache_page_t **,
    page_cache_page_t **, size_t *, size_t *, size_t *,
    size_t,
    uint64_t (*)(void *), void (*)(void *, uint64_t),
    uint64_t (*)(void *, uint32_t), void (*)(void *, uint32_t, uint64_t),
    int (*)(void *, int), page_cache_page_t *(*)(void *, vnode_t **),
    int (*)(void *, size_t), void (*)(void *, page_cache_page_t *),
    void (*)(void *, void *), void (*)(void *, void *),
    int (*)(uint32_t), void (*)(size_t));
extern long a20_pagecache_read_transaction(
    void *, size_t, size_t, uint8_t *, size_t,
    page_cache_page_t *(*)(void *, uint64_t, int),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *, size_t),
    int (*)(void *, uint64_t), void (*)(void *, uint64_t),
    void (*)(void *, page_cache_page_t *), void (*)(void *, uint64_t));
extern long a20_pagecache_write_transaction(
    void *, size_t, size_t, const uint8_t *, size_t,
    page_cache_page_t *(*)(void *, uint64_t, int),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    void (*)(void *, page_cache_page_t *), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t), void (*)(void *, size_t));
extern void a20_pagecache_update_after_write(
    void *, uint64_t, uint64_t, const uint8_t *, size_t,
    page_cache_page_t *(*)(void *, uint64_t, int),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *),
    void (*)(void *, page_cache_page_t *), void (*)(void *, size_t));
extern page_cache_page_t *a20_pagecache_evict_scan(
    page_cache_page_t *lru_head, page_cache_page_t *lru_tail, size_t budget,
    int (*pfn_reclaimable)(uint32_t), uint64_t (*lock_bucket)(uint32_t),
    void (*unlock_bucket)(uint32_t, uint64_t), page_cache_page_t **hash,
    page_cache_page_t **global_dirty_head, size_t *global_dirty_count,
    size_t *valid_pages, void (*account)(size_t), vnode_t **out_vnode);
extern size_t a20_pagecache_collect_dirty(vnode_t *vn, page_cache_page_t *global_dirty_head,
                                          size_t max_pages, page_cache_page_t **out,
                                          size_t *bucket_pinned, void (*account)(size_t));
extern size_t a20_pagecache_writeback_confirm(page_cache_page_t **pages, size_t count,
                                              vnode_t *batch_vn, uint64_t index,
                                              const uint64_t *gens,
                                              page_cache_page_t **global_dirty_head,
                                              size_t *global_dirty_count);
extern size_t a20_pagecache_detach_unpinned_range(
    vnode_t *vn, uint64_t first, uint64_t last, int require_clean,
    uint64_t (*lock_bucket)(uint32_t),
    void (*unlock_bucket)(uint32_t, uint64_t), page_cache_page_t **hash,
    page_cache_page_t **global_dirty_head, size_t *global_dirty_count,
    size_t *valid_pages, void (*account)(size_t), size_t *out_discarded_dirty);
extern size_t a20_pagecache_truncate(
    vnode_t *vn, uint64_t new_size, uint64_t (*lock_bucket)(uint32_t),
    void (*unlock_bucket)(uint32_t, uint64_t), page_cache_page_t **hash,
    page_cache_page_t **global_dirty_head, size_t *global_dirty_count,
    size_t *valid_pages, void (*account)(size_t));
extern int a20_pagecache_readahead_plan(int is_regular, uint64_t file_size,
                                       uint64_t start, size_t count,
                                       size_t *out_count);
extern int a20_pagecache_write_plan(uint64_t file_size, uint64_t start,
                                    size_t done, size_t count, uint64_t *out_index,
                                    size_t *out_page_off, size_t *out_chunk,
                                    int *out_may_fill);
extern int a20_pagecache_writeback_transaction(
    vnode_t *vn, page_cache_writepage_t writepage, void *ctx,
    size_t max_pages, size_t *written_pages,
    size_t (*collect)(vnode_t *, page_cache_page_t **, size_t),
    void (*lock_pages)(page_cache_page_t **, size_t),
    void (*unlock_pages)(page_cache_page_t **, size_t),
    int (*batch_allowed)(vnode_t *, size_t),
    int (*write)(vnode_t *, uint64_t, page_cache_page_t **, size_t,
                 page_cache_writepage_t, void *, void *),
    void *(*alloc_buffer)(size_t), void (*free_buffer)(void *),
    void (*confirm)(page_cache_page_t **, size_t, vnode_t *, uint64_t,
                    const uint64_t *),
    void (*put)(page_cache_page_t *),
    void (*serial_lock)(vnode_t *), void (*serial_unlock)(vnode_t *),
    void (*perf)(size_t));
extern int a20_pagecache_fill_one_transaction(
    void *, page_cache_page_t *, void (*)(void *, page_cache_page_t *),
    void (*)(void *, page_cache_page_t *), int (*)(void *, page_cache_page_t *),
    uint64_t (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *, uint64_t),
    int (*)(void *, page_cache_page_t *, uint8_t *));
extern int a20_pagecache_fill_many_transaction(
    void *, page_cache_page_t **, size_t, size_t,
    void (*)(void *, page_cache_page_t *), void (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *), uint64_t (*)(void *, page_cache_page_t *),
    int (*)(void *, page_cache_page_t *, uint64_t),
    int (*)(void *, page_cache_page_t *, uint8_t *),
    int (*)(void *, uint64_t, uint8_t *, size_t),
    uint8_t *(*)(void *, size_t), void (*)(void *, uint8_t *));
extern int a20_pagecache_readahead_transaction(
    void *, page_cache_page_t *, size_t, size_t,
    page_cache_page_t *(*)(void *, uint64_t, int),
    int (*)(void *, page_cache_page_t **, size_t),
    void (*)(void *, page_cache_page_t *), void (*)(void *, size_t));
extern vnode_t *a20_pagecache_reclaim_clean(
    page_cache_page_t *page, uint64_t (*lock_bucket)(uint32_t),
    void (*unlock_bucket)(uint32_t, uint64_t), int (*pfn_reclaimable)(uint32_t),
    page_cache_page_t **hash, page_cache_page_t **global_dirty_head,
    size_t *global_dirty_count, size_t *valid_pages, page_cache_page_t **free_head,
    void (*account)(size_t));

/* The Rust engine mirrors the descriptor prefix through dirty_parent_color and
 * the leading vnode cache fields; these asserts fail the build on drift. */
_Static_assert(__builtin_offsetof(page_cache_page_t, index) == 8,
               "page cache state mirror: index offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, ref_count) == 16,
               "page cache state mirror: ref_count offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, dirty_gen) == 32,
               "page cache state mirror: dirty_gen offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, uptodate) == 48,
               "page cache state mirror: uptodate offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, accessed) == 52,
               "page cache state mirror: accessed offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, prev) > 52,
               "page cache state mirror: links follow the scalar prefix");
_Static_assert(__builtin_offsetof(vnode_t, ino) == 0,
               "page cache state mirror: vnode.ino offset");
#if __SIZEOF_POINTER__ == 8
_Static_assert(__builtin_offsetof(vnode_t, mnt) == 48,
               "page cache state mirror: vnode.mnt offset (LP64)");
_Static_assert(__builtin_offsetof(vnode_t, cache_pages) == 72,
               "page cache state mirror: vnode.cache_pages offset (LP64)");
_Static_assert(__builtin_offsetof(page_cache_page_t, accessed) == 52,
               "page cache state mirror: accessed offset (LP64)");
_Static_assert(__builtin_offsetof(page_cache_page_t, pfn) == 56,
               "page cache state mirror: pfn offset (LP64)");
_Static_assert(__builtin_offsetof(page_cache_page_t, prev) == 72,
               "page cache state mirror: prev offset (LP64)");
#else
_Static_assert(__builtin_offsetof(vnode_t, mnt) == 36,
               "page cache state mirror: vnode.mnt offset (ILP32)");
_Static_assert(__builtin_offsetof(vnode_t, cache_pages) == 48,
               "page cache state mirror: vnode.cache_pages offset (ILP32)");
#endif

_Static_assert(__alignof__(page_cache_page_t) >= 2,
               "page cache RB parent/color requires aligned descriptors");
_Static_assert(__builtin_offsetof(page_cache_page_t, dirty_children) ==
               __builtin_offsetof(page_cache_page_t, global_dirty_next) + sizeof(void *),
               "page cache state mirror: dirty_children offset");
_Static_assert(__builtin_offsetof(page_cache_page_t, dirty_parent_color) ==
               __builtin_offsetof(page_cache_page_t, dirty_children) + 2 * sizeof(void *),
               "page cache state mirror: dirty_parent_color offset");
_Static_assert(__builtin_offsetof(vnode_t, cache_dirty_root) ==
               __builtin_offsetof(vnode_t, cache_dirty_tail) + sizeof(void *),
               "page cache state mirror: vnode.cache_dirty_root offset");

static spinlock_t g_page_cache_lock = SPINLOCK_INIT;
static spinlock_t g_page_cache_bucket_locks[PAGE_CACHE_BUCKET_LOCKS];
static mutex_t g_page_cache_grow_lock;
/* Writeback serialization.  A single mutex made every fsync (and cache
 * pressure writeback) from eight parallel processes serialize, even when they
 * flush disjoint vnodes.  The global mutex is kept for whole-cache writeback
 * (vn == NULL); per-vnode writeback takes a hashed per-vnode mutex so
 * concurrent fsyncs of different files proceed in parallel.  Two writebacks
 * of the same vnode still serialize and the dirty-gen publish/clear path is
 * unchanged. */
#define PAGE_CACHE_WRITEBACK_LOCKS 64
static mutex_t g_page_cache_writeback_lock;
static mutex_t g_page_cache_writeback_locks[PAGE_CACHE_WRITEBACK_LOCKS];
/*
 * The vfile fill path reaches the filesystem through vf->ops->read/lseek, which
 * the syscall layer protects with vf->offset_lock but which the fill calls
 * directly.  Two threads filling different pages of one file would otherwise
 * interleave on the shared vf->offset and read from the wrong file position, so
 * fills are striped by vfile pointer here (every page of one file lands on the
 * same lock).  vf->offset_lock itself cannot be reused: a fault taken inside a
 * read() syscall re-enters this path and would deadlock on that mutex.
 */
#define PAGE_CACHE_FILL_LOCKS 16
static mutex_t g_page_cache_fill_locks[PAGE_CACHE_FILL_LOCKS];
#define PAGE_CACHE_CHUNKS \
    (PAGE_CACHE_MAX_PAGES / PAGE_CACHE_CHUNK_PAGES)
static page_cache_page_t *g_page_chunks[PAGE_CACHE_CHUNKS];
/* Staging buffers for readpages(), checked out instead of allocated per
 * window.  A per-task buffer of the kind the Linux ABI I/O paths use cannot be
 * borrowed here: sys_read hands its own scratch buffer in as the copy
 * destination, so a shared per-task allocation would be freed underneath the
 * writer.  Checkout also survives preemption, which a bare per-CPU buffer
 * would not.  Allocated lazily on first use and retained afterwards. */
#define PAGE_CACHE_READAHEAD_BUFFERS 4
static void *g_readahead_buffers[PAGE_CACHE_READAHEAD_BUFFERS];
static spinlock_t g_readahead_buffer_lock = SPINLOCK_INIT;
/* End of the previous read(2), per open file, for the readahead gate below. */
#define PAGE_CACHE_READ_STATE_STRIPES 64
struct page_cache_read_state {
    uintptr_t vfile;
    uint64_t end_index;
};
static struct page_cache_read_state
    g_read_state[PAGE_CACHE_READ_STATE_STRIPES];
static size_t g_allocated_pages;
static size_t g_page_limit;
static page_cache_page_t *g_free_pages;
static page_cache_page_t g_lru_head;
static page_cache_page_t g_lru_tail;
static page_cache_page_t *g_hash[PAGE_CACHE_HASH_BUCKETS];
static page_cache_page_t *g_dirty_pages;
/* Whole-cache occupancy, maintained at the points already serialised by
 * g_page_cache_lock (mapping publication, detach, dirty insert/remove).
 * Deriving it by walking the descriptor array made every cachestat(2) and
 * /proc/meminfo read an O(cache size) critical section with interrupts off. */
static size_t g_valid_pages;
static size_t g_dirty_page_count;
/* Pages currently carrying a pin, one counter per bucket.  The 0->1 and 1->0
 * transitions belong to the warm hit path, which already owns that bucket's
 * lock; a single shared counter would make unrelated files contend on one
 * cache line.  Padded so neighbouring buckets do not false-share. */
struct page_cache_pin_counter {
    size_t pinned;
    char pad[64 - sizeof(size_t)];
};
static struct page_cache_pin_counter
    g_bucket_pinned[PAGE_CACHE_BUCKET_LOCKS];
static int g_initialized;

#define PAGE_CACHE_PRESSURE_WRITEBACK_PAGES 1024U

static int page_cache_writeback_some(size_t max_pages);
static page_cache_page_t *page_cache_file_get(void *, uint64_t, int);
static void page_cache_file_put(void *, page_cache_page_t *);
static int page_cache_fill_window(void *, page_cache_page_t **, size_t);
static void page_cache_readahead_perf(void *, size_t);

static inline void page_cache_account_scan(size_t entries)
{
    a20_perf_count(A20_PERF_PAGE_CACHE_SCAN_CALLS);
    a20_perf_add(A20_PERF_PAGE_CACHE_SCAN_ENTRIES, entries);
}

_Static_assert(sizeof(struct page_cache_pin_counter) == 64,
               "page cache state mirror: pin counter stride");
_Static_assert(__builtin_offsetof(struct page_cache_pin_counter, pinned) == 0,
               "page cache state mirror: pin counter lead field");

/* Narrow mechanical bridges the Rust engine calls back into: telemetry,
 * reclaimability of a frame, and the bucket spinlock pair. */
static void page_cache_state_scan_bridge(size_t entries)
{
    page_cache_account_scan(entries);
}

static int page_cache_state_pfn_reclaimable(uint32_t pfn)
{
    return pfn_valid(pfn) && pfa.meta[pfn].refcount <= 1;
}

static uint64_t page_cache_state_bucket_lock(uint32_t blk)
{
    return spin_lock_irqsave(&g_page_cache_bucket_locks[blk]);
}

static void page_cache_state_bucket_unlock(uint32_t blk, uint64_t flags)
{
    spin_unlock_irqrestore(&g_page_cache_bucket_locks[blk], flags);
}

/* Dedicated wrappers keep the Rust ABI free of inline-lock expansions and
 * give one place to add a lockdep annotation if the lock order changes. */

/* Lock ordering: a caller may hold g_page_cache_lock and then acquire a bucket
 * lock, but must never acquire g_page_cache_lock while holding a bucket lock.
 * The warm hit path acquires only the bucket lock. */
static void lru_insert_tail(page_cache_page_t *page)
{
    a20_pagecache_lru_insert_tail(&g_lru_tail, page);
}

static page_cache_page_t *page_cache_page_at(size_t index)
{
    if (index >= g_allocated_pages)
        return NULL;
    return &g_page_chunks[index / PAGE_CACHE_CHUNK_PAGES]
                         [index % PAGE_CACHE_CHUNK_PAGES];
}

static void free_insert_locked(page_cache_page_t *page)
{
    a20_pagecache_free_insert(&g_free_pages, page);
}

static void dirty_insert_locked(page_cache_page_t *page)
{
    a20_pagecache_dirty_insert(page, &g_dirty_pages, &g_dirty_page_count);
}

static void dirty_remove_locked(page_cache_page_t *page)
{
    a20_pagecache_dirty_remove(page, &g_dirty_pages, &g_dirty_page_count);
}

/* Caller holds g_page_cache_lock AND the page's bucket lock.  Removing the
 * mapping pins and unlinks the page from hash, per-vnode mapping and dirty
 * lists in one critical section so a concurrent bucket-lock hit can neither
 * observe a half-detached page nor pin a page we are about to reclaim. */

/*
 * Caller holds g_page_cache_lock.  Second-chance eviction: the global LRU
 * gives a rough insertion order; a candidate's bucket lock makes the
 * refcount/detach decision atomic against a concurrent warm hit, and the
 * accessed bit lets recently-used pages survive one eviction sweep without
 * any per-hit global LRU mutation.
 *
 * The sweep is bounded.  A run of recently-used candidates costs one bucket
 * lock acquisition each, and an unbounded scan turned one allocation miss into
 * an O(cache size) critical section that stalled every other miss, evict and
 * statistics caller.  Giving up early is not a lost opportunity: the accessed
 * bits of the candidates already visited are cleared, so the caller's next
 * sweep reaches pages this one had to skip.
 */
#define PAGE_CACHE_EVICT_SCAN_BUDGET 128U
/* Extra sweeps one allocation may spend before escalating.  Each is a separate
 * bounded critical section, so the worst case is a fixed number of short
 * sections rather than one long one. */
#define PAGE_CACHE_EVICT_SWEEPS 3U

static page_cache_page_t *evict_locked(vnode_t **deferred_put)
{
    vnode_t *deferred = NULL;
    page_cache_page_t *page = a20_pagecache_evict_scan(
        &g_lru_head, &g_lru_tail, PAGE_CACHE_EVICT_SCAN_BUDGET,
        page_cache_state_pfn_reclaimable, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge,
        &deferred);
    *deferred_put = deferred;
    return page;
}

/* Allocate descriptors and frames in bounded chunks.  The old fixed cache
 * reserved 8 MiB at boot and thrashed on every large compiler image.  Lazy
 * chunks retain a large clean-file working set when memory is available while
 * leaving untouched systems at the old initial footprint. */
static int page_cache_grow_internal(int force)
{
    mutex_lock(&g_page_cache_grow_lock);
    uint64_t cache_flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t ceiling = force ? PAGE_CACHE_MAX_PAGES : g_page_limit;
    int growth_unneeded = g_allocated_pages >= ceiling ||
                          (g_initialized && g_free_pages != NULL);
    spin_unlock_irqrestore(&g_page_cache_lock, cache_flags);
    if (growth_unneeded) {
        mutex_unlock(&g_page_cache_grow_lock);
        return g_allocated_pages >= ceiling ? -ENOSPC : 0;
    }

    page_cache_page_t *chunk =
        kcalloc(PAGE_CACHE_CHUNK_PAGES, sizeof(page_cache_page_t));
    if (!chunk) {
        mutex_unlock(&g_page_cache_grow_lock);
        return -ENOMEM;
    }

    size_t initialized = 0;
    for (; initialized < PAGE_CACHE_CHUNK_PAGES; initialized++) {
        chunk[initialized].pfn = pfa_alloc_page();
        if (chunk[initialized].pfn == PFN_NONE)
            break;
        chunk[initialized].data = pfn_to_virt(chunk[initialized].pfn);
        refcount_set(&chunk[initialized].ref_count, 0);
        mutex_init(&chunk[initialized].fill_lock);
    }
    if (initialized != PAGE_CACHE_CHUNK_PAGES) {
        for (size_t i = 0; i < initialized; i++)
            frame_put(chunk[i].pfn);
        kfree(chunk);
        mutex_unlock(&g_page_cache_grow_lock);
        return -ENOMEM;
    }

    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    if (g_allocated_pages / PAGE_CACHE_CHUNK_PAGES >= PAGE_CACHE_CHUNKS) {
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        mutex_unlock(&g_page_cache_grow_lock);
        return -ENOSPC;
    }
    size_t chunk_index = g_allocated_pages / PAGE_CACHE_CHUNK_PAGES;
    g_page_chunks[chunk_index] = chunk;
    for (size_t i = 0; i < PAGE_CACHE_CHUNK_PAGES; i++) {
        chunk[i].accessed = 0;
        lru_insert_tail(&chunk[i]);
        free_insert_locked(&chunk[i]);
    }
    g_allocated_pages += PAGE_CACHE_CHUNK_PAGES;
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    mutex_unlock(&g_page_cache_grow_lock);
    return 0;
}

static int page_cache_grow(void)
{
    return page_cache_grow_internal(0);
}

int page_cache_init(void)
{
    if (g_initialized)
        return 0;

    spin_init(&g_page_cache_lock);
    spin_init(&g_readahead_buffer_lock);
    for (size_t i = 0; i < PAGE_CACHE_BUCKET_LOCKS; i++)
        spin_init(&g_page_cache_bucket_locks[i]);
    mutex_init(&g_page_cache_grow_lock);
    mutex_init(&g_page_cache_writeback_lock);
    for (size_t i = 0; i < PAGE_CACHE_WRITEBACK_LOCKS; i++)
        mutex_init(&g_page_cache_writeback_locks[i]);
    for (size_t i = 0; i < PAGE_CACHE_FILL_LOCKS; i++)
        mutex_init(&g_page_cache_fill_locks[i]);
    lock_counters_register(&g_page_cache_lock, "page_cache");
    /* Keep the cache bounded to one eighth of RAM on small normal boots while
     * retaining a 1 GiB ceiling on 8 GiB hosts. */
    g_page_limit = pfa.total_frames / 8;
    if (g_page_limit < PAGE_CACHE_INITIAL_PAGES)
        g_page_limit = PAGE_CACHE_INITIAL_PAGES;
    if (g_page_limit > PAGE_CACHE_MAX_PAGES)
        g_page_limit = PAGE_CACHE_MAX_PAGES;
    g_page_limit -= g_page_limit % PAGE_CACHE_CHUNK_PAGES;

    g_lru_head.prev = NULL;
    g_lru_head.next = &g_lru_tail;
    g_lru_tail.prev = &g_lru_head;
    g_lru_tail.next = NULL;

    for (size_t i = 0; i < PAGE_CACHE_INITIAL_PAGES;
         i += PAGE_CACHE_CHUNK_PAGES) {
        if (page_cache_grow() < 0)
            return -ENOMEM;
    }
    g_initialized = 1;
    return 0;
}

static uint64_t page_cache_get_global_lock(void *ctx)
{ (void)ctx; return spin_lock_irqsave(&g_page_cache_lock); }
static void page_cache_get_global_unlock(void *ctx, uint64_t flags)
{ (void)ctx; spin_unlock_irqrestore(&g_page_cache_lock, flags); }
static uint64_t page_cache_get_bucket_lock(void *ctx, uint32_t bucket)
{ (void)ctx; return page_cache_state_bucket_lock(bucket); }
static void page_cache_get_bucket_unlock(void *ctx, uint32_t bucket, uint64_t flags)
{ (void)ctx; page_cache_state_bucket_unlock(bucket, flags); }
static int page_cache_get_grow(void *ctx, int force)
{ (void)ctx; return page_cache_grow_internal(force); }
static page_cache_page_t *page_cache_get_evict(void *ctx, vnode_t **deferred)
{ (void)ctx; return evict_locked(deferred); }
static int page_cache_get_pressure_writeback(void *ctx, size_t count)
{ (void)ctx; return page_cache_writeback_some(count); }
static void page_cache_get_zero(void *ctx, page_cache_page_t *page)
{ (void)ctx; memset(page->data, 0, PAGE_SIZE); }
static void page_cache_get_vnode_get(void *ctx, void *vn)
{ (void)ctx; vnode_get(vn); }
static void page_cache_get_vnode_put(void *ctx, void *vn)
{ (void)ctx; vnode_put(vn); }

page_cache_page_t *page_cache_get(vnode_t *vn, uint64_t index, int create)
{
    if (!g_initialized || !vn)
        return NULL;
    return a20_pagecache_get_transaction(
        &g_page_cache_lock, vn, index, create, g_hash, &g_lru_head, &g_lru_tail,
        &g_free_pages, &g_dirty_pages, &g_dirty_page_count, &g_valid_pages,
        (size_t *)g_bucket_pinned, PAGE_CACHE_PRESSURE_WRITEBACK_PAGES,
        page_cache_get_global_lock,
        page_cache_get_global_unlock, page_cache_get_bucket_lock,
        page_cache_get_bucket_unlock, page_cache_get_grow,
        page_cache_get_evict, page_cache_get_pressure_writeback,
        page_cache_get_zero, page_cache_get_vnode_get,
        page_cache_get_vnode_put, page_cache_state_pfn_reclaimable,
        page_cache_state_scan_bridge);
}

void page_cache_put(page_cache_page_t *page)
{
    (void)a20_pagecache_put(page, (size_t *)g_bucket_pinned);
}

void *page_cache_data(page_cache_page_t *page)
{
    return page ? page->data : NULL;
}

void page_cache_mark_uptodate(page_cache_page_t *page)
{
    if (page)
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
}

void page_cache_mark_dirty(page_cache_page_t *page)
{
    if (!page)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    if (page->valid) {
        /* Dirty data is authoritative even after an earlier invalidation. */
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&page->dirty_gen, 1, __ATOMIC_RELEASE);
        dirty_insert_locked(page);
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

void page_cache_mark_clean(page_cache_page_t *page)
{
    if (!page)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    dirty_remove_locked(page);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

int page_cache_is_uptodate(page_cache_page_t *page)
{
    if (!page)
        return 0;
    return __atomic_load_n(&page->uptodate, __ATOMIC_ACQUIRE) != 0;
}

static int publish_uptodate(page_cache_page_t *page, uint64_t invalidate_gen)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    int unchanged = page->valid && page->invalidate_gen == invalidate_gen;
    if (unchanged)
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    return unchanged;
}

static uint64_t snapshot_invalidate_gen(page_cache_page_t *page)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    uint64_t invalidate_gen = page->invalidate_gen;
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    return invalidate_gen;
}

static void page_cache_fill_lock(void *ctx, page_cache_page_t *page)
{ (void)ctx; mutex_lock(&page->fill_lock); }

static void page_cache_fill_unlock(void *ctx, page_cache_page_t *page)
{ (void)ctx; mutex_unlock(&page->fill_lock); }

static int page_cache_fill_ready(void *ctx, page_cache_page_t *page)
{ (void)ctx; return page_cache_is_uptodate(page); }

static uint64_t page_cache_fill_generation(void *ctx, page_cache_page_t *page)
{ (void)ctx; return snapshot_invalidate_gen(page); }

static int page_cache_fill_publish(void *ctx, page_cache_page_t *page,
                                   uint64_t generation)
{ (void)ctx; return publish_uptodate(page, generation); }

static int page_cache_fill_read_one(void *ctx, page_cache_page_t *page,
                                    uint8_t *data)
{
    vfile_t *vf = ctx;
    if (vf->vnode->ops && vf->vnode->ops->readpage)
        return vf->vnode->ops->readpage(vf->vnode, page->index,
                                        data, PAGE_SIZE);

    mutex_t *fill_offset_lock =
        &g_page_cache_fill_locks[((uintptr_t)vf >> 4) & (PAGE_CACHE_FILL_LOCKS - 1)];
    mutex_lock(fill_offset_lock);
    size_t saved = vf->offset;
    long seek_r = vf->ops->lseek(vf, (long)(page->index * PAGE_SIZE), SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(fill_offset_lock);
        return (int)seek_r;
    }
    int result = vf->ops->read(vf, (char *)data, PAGE_SIZE);
    long restore_r = vf->ops->lseek(vf, (long)saved, SEEK_SET);
    mutex_unlock(fill_offset_lock);
    if (restore_r < 0 && result >= 0)
        result = (int)restore_r;
    return result;
}

int page_cache_fill_vfile_page(vfile_t *vf, page_cache_page_t *page)
{
    if (!vf || !vf->ops || !vf->ops->read || !vf->ops->lseek)
        return -EINVAL;
    return a20_pagecache_fill_one_transaction(
        vf, page, page_cache_fill_lock, page_cache_fill_unlock,
        page_cache_fill_ready, page_cache_fill_generation,
        page_cache_fill_publish, page_cache_fill_read_one);
}

/* Fill an ascending, contiguous private-file fault window.  Filesystems with
 * readpages support receive a linear buffer and can merge physical disk I/O;
 * the data is scattered into the pinned cache pages only after the read. */
static void *readahead_buffer_take(size_t bytes)
{
    uint64_t flags = spin_lock_irqsave(&g_readahead_buffer_lock);
    for (size_t i = 0; i < PAGE_CACHE_READAHEAD_BUFFERS; i++) {
        if (g_readahead_buffers[i]) {
            void *buffer = g_readahead_buffers[i];
            g_readahead_buffers[i] = NULL;
            spin_unlock_irqrestore(&g_readahead_buffer_lock, flags);
            return buffer;
        }
    }
    spin_unlock_irqrestore(&g_readahead_buffer_lock, flags);
    return kmalloc(bytes);
}

static void readahead_buffer_return(void *buffer)
{
    uint64_t flags = spin_lock_irqsave(&g_readahead_buffer_lock);
    for (size_t i = 0; i < PAGE_CACHE_READAHEAD_BUFFERS; i++) {
        if (!g_readahead_buffers[i]) {
            g_readahead_buffers[i] = buffer;
            spin_unlock_irqrestore(&g_readahead_buffer_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&g_readahead_buffer_lock, flags);
    kfree(buffer);
}

static int page_cache_fill_read_many(void *ctx, uint64_t first_index,
                                     uint8_t *buffer, size_t bytes)
{
    vfile_t *vf = ctx;
    return vf->vnode->ops->readpages(vf->vnode, first_index, buffer, bytes);
}

static uint8_t *page_cache_fill_take_buffer(void *ctx, size_t bytes)
{ (void)ctx; return readahead_buffer_take(bytes); }

static void page_cache_fill_return_buffer(void *ctx, uint8_t *buffer)
{ (void)ctx; readahead_buffer_return(buffer); }

int page_cache_fill_vfile_pages(vfile_t *vf, page_cache_page_t **pages,
                                size_t count)
{
    if (!vf || !vf->vnode || !pages || count == 0)
        return -EINVAL;
    if (!vf->vnode->ops || !vf->vnode->ops->readpages) {
        if (!vf->ops || !vf->ops->read || !vf->ops->lseek)
            return -EINVAL;
    }
    return a20_pagecache_fill_many_transaction(
        vf, pages, count, PAGE_CACHE_READAHEAD_PAGES,
        page_cache_fill_lock, page_cache_fill_unlock, page_cache_fill_ready,
        page_cache_fill_generation, page_cache_fill_publish,
        page_cache_fill_read_one,
        vf->vnode->ops && vf->vnode->ops->readpages ?
            page_cache_fill_read_many : NULL,
        page_cache_fill_take_buffer, page_cache_fill_return_buffer);
}

/*
 * Ordinary read(2) used to fill one 4 KiB page at a time even when the
 * filesystem provided readpages().  Compiler inputs are predominantly
 * sequential and the ext4 implementation can merge a contiguous 128 KiB
 * window into one block request, so populate the forward window on a cold
 * page.  The caller already pins pages[0]; pins acquired here are dropped
 * before returning and all publication remains protected by the existing
 * per-page fill locks.
 */
static int page_cache_readahead_vfile(vfile_t *vf,
                                      page_cache_page_t *first,
                                      size_t file_size)
{
    return a20_pagecache_readahead_transaction(
        vf, first, file_size, PAGE_CACHE_READAHEAD_PAGES,
        page_cache_file_get, page_cache_fill_window, page_cache_file_put,
        page_cache_readahead_perf);
}

/* Where the previous read(2) on this open file stopped, striped by vfile
 * pointer.  The window is worth its read amplification only when the stream
 * is walking forward: an unconditional window made a 4 KiB random read pay for
 * 32 pages, of which 31 were never looked at.  Two indices equal means this
 * read resumes exactly where the last one ended, which is what a sequential
 * scan, and only a sequential scan, produces.  The first page of a stream has
 * no evidence and fetches alone; the second page already has it.
 *
 * A collision between two vfiles sharing a stripe, or a torn read of the pair,
 * can only mis-fire the hint into an unnecessary or a missed prefetch.  It
 * cannot affect correctness, which is why the two fields are not published
 * atomically. */
static int page_cache_read_resumes(vfile_t *vf, uint64_t index)
{
    struct page_cache_read_state *s = &g_read_state[
        ((uintptr_t)vf >> 4) & (PAGE_CACHE_READ_STATE_STRIPES - 1)];
    if (__atomic_load_n(&s->vfile, __ATOMIC_ACQUIRE) != (uintptr_t)vf)
        return 0;
    return __atomic_load_n(&s->end_index, __ATOMIC_RELAXED) == index;
}

static void page_cache_record_read_end(vfile_t *vf, uint64_t index)
{
    struct page_cache_read_state *s = &g_read_state[
        ((uintptr_t)vf >> 4) & (PAGE_CACHE_READ_STATE_STRIPES - 1)];
    __atomic_store_n(&s->vfile, (uintptr_t)vf, __ATOMIC_RELAXED);
    __atomic_store_n(&s->end_index, index + 1, __ATOMIC_RELEASE);
}

pfn_t page_cache_pfn(page_cache_page_t *page)
{
    return page ? page->pfn : PFN_NONE;
}

static page_cache_page_t *page_cache_file_get(void *ctx, uint64_t index, int create)
{ vfile_t *vf=ctx; return page_cache_get(vf->vnode,index,create); }
static int page_cache_fill_window(void *ctx, page_cache_page_t **pages,
                                  size_t count)
{ return page_cache_fill_vfile_pages(ctx, pages, count); }
static void page_cache_readahead_perf(void *ctx, size_t count)
{
    (void)ctx;
    a20_perf_count(A20_PERF_PAGE_CACHE_READAHEAD_CALLS);
    a20_perf_add(A20_PERF_PAGE_CACHE_READAHEAD_PAGES, count);
}
static page_cache_page_t *page_cache_update_get(void *ctx, uint64_t index, int create)
{ return page_cache_get(ctx,index,create); }
static int page_cache_file_ready(void *ctx, page_cache_page_t *page)
{ (void)ctx; return page_cache_is_uptodate(page); }
static int page_cache_file_fill(void *ctx, page_cache_page_t *page)
{ return page_cache_fill_vfile_page(ctx,page); }
static int page_cache_file_readahead(void *ctx, page_cache_page_t *page, size_t size)
{ return page_cache_readahead_vfile(ctx,page,size); }
static int page_cache_file_resumes(void *ctx, uint64_t index)
{ vfile_t *vf=ctx; return vf->vnode->ops && vf->vnode->ops->readpages && page_cache_read_resumes(vf,index); }
static void page_cache_file_record_end(void *ctx, uint64_t index)
{ page_cache_record_read_end(ctx,index); }
static void page_cache_file_put(void *ctx, page_cache_page_t *page)
{ (void)ctx; page_cache_put(page); }
static void page_cache_file_seek(void *ctx, uint64_t offset)
{
    vfile_t *vf=ctx;
    if (vf->ops->lseek(vf,(long)offset,SEEK_SET)<0) vf->offset=offset;
}
static int page_cache_file_lock_page(void *ctx, page_cache_page_t *page)
{ (void)ctx; mutex_lock(&page->fill_lock); return 0; }
static int page_cache_file_unlock_page(void *ctx, page_cache_page_t *page)
{ (void)ctx; mutex_unlock(&page->fill_lock); return 0; }
static int page_cache_file_mark_dirty(void *ctx, page_cache_page_t *page)
{ (void)ctx; page_cache_mark_dirty(page); return 0; }
static int page_cache_file_mark_clean(void *ctx, page_cache_page_t *page)
{ (void)ctx; page_cache_mark_clean(page); return 0; }
static int page_cache_file_zero(void *ctx, page_cache_page_t *page)
{ (void)ctx; memset(page->data,0,PAGE_SIZE); return 0; }
static int page_cache_file_mark_uptodate(void *ctx, page_cache_page_t *page)
{ (void)ctx; page_cache_mark_uptodate(page); return 0; }
static void page_cache_file_write_commit(void *ctx, uint64_t end)
{ vfile_t *vf=ctx; if(end>vf->vnode->size) vf->vnode->size=end; }
static void page_cache_file_write_perf(void *ctx, size_t updated)
{
    (void)ctx; a20_perf_count(A20_PERF_PCACHE_WRITE_UPDATES);
    a20_perf_add(A20_PERF_PCACHE_WRITE_UPDATE_PAGES,updated);
}

int page_cache_read_vfile(vfile_t *vf, char *buf, size_t count)
{
    if (!vf || !vf->vnode || !buf)
        return -EINVAL;
    if (!vf->ops || !vf->ops->read || !vf->ops->lseek)
        return -ENOSYS;
    if (vf->vnode->type != VFS_FT_REGULAR)
        return -EINVAL;
    if (count == 0)
        return 0;

    return (int)a20_pagecache_read_transaction(
        vf,vf->vnode->size,vf->offset,(uint8_t *)buf,count,
        page_cache_file_get,page_cache_file_ready,page_cache_file_fill,
        page_cache_file_readahead,page_cache_file_resumes,
        page_cache_file_record_end,page_cache_file_put,page_cache_file_seek);
}

int page_cache_write_vfile(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf || !vf->vnode || !buf)
        return -EINVAL;
    if (!vf->ops || !vf->ops->lseek ||
        !vf->vnode->ops || !vf->vnode->ops->writepage)
        return -ENOSYS;
    if (vf->vnode->type != VFS_FT_REGULAR)
        return -EINVAL;
    if (count == 0)
        return 0;

    return (int)a20_pagecache_write_transaction(
        vf,vf->vnode->size,vf->offset,(const uint8_t *)buf,count,
        page_cache_file_get,page_cache_file_ready,page_cache_file_fill,
        page_cache_file_lock_page,page_cache_file_unlock_page,
        page_cache_file_mark_dirty,page_cache_file_zero,
        page_cache_file_mark_uptodate,page_cache_file_put,
        page_cache_file_seek,page_cache_file_write_commit,page_cache_file_write_perf);
}

static size_t collect_dirty_batch_locked(vnode_t *vn,
                                         page_cache_page_t **pages,
                                         size_t max_pages)
{
    return a20_pagecache_collect_dirty(vn, g_dirty_pages, max_pages, pages,
                                       (size_t *)g_bucket_pinned,
                                       page_cache_state_scan_bridge);
}

static size_t page_cache_writeback_collect(vnode_t *vn,
                                           page_cache_page_t **pages,
                                           size_t max_pages)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t count = collect_dirty_batch_locked(vn, pages, max_pages);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    return count;
}

static void page_cache_writeback_lock_pages(page_cache_page_t **pages,
                                            size_t count)
{
    for (size_t i = 0; i < count; i++)
        mutex_lock(&pages[i]->fill_lock);
}

static void page_cache_writeback_unlock_pages(page_cache_page_t **pages,
                                              size_t count)
{
    while (count > 0)
        mutex_unlock(&pages[--count]->fill_lock);
}

static int page_cache_writeback_batch_allowed(vnode_t *vn, size_t count)
{
    return vn && vn->ops && vn->ops->writepages && count > 1;
}

static int page_cache_writeback_write(vnode_t *vn, uint64_t index,
                                      page_cache_page_t **pages, size_t count,
                                      page_cache_writepage_t writepage,
                                      void *linear, void *ctx)
{
    if (writepage)
        return writepage(vn, index, pages[0]->data, PAGE_SIZE, ctx);
    if (vn && vn->ops && vn->ops->writepages && count > 1) {
        if (!linear)
            return -ENOMEM;
        for (size_t i = 0; i < count; i++)
            memcpy((char *)linear + i * PAGE_SIZE, pages[i]->data, PAGE_SIZE);
        return vn->ops->writepages(vn, index, linear, count * PAGE_SIZE);
    }
    if (vn && vn->ops && vn->ops->writepage)
        return vn->ops->writepage(vn, index, pages[0]->data, PAGE_SIZE);
    (void)ctx;
    return -ENOSYS;
}

static void *page_cache_writeback_alloc(size_t size)
{
    return kmalloc(size);
}

static void page_cache_writeback_free(void *ptr)
{
    kfree(ptr);
}

static void page_cache_writeback_confirm(page_cache_page_t **pages,
                                         size_t count, vnode_t *vn,
                                         uint64_t index, const uint64_t *gens)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    a20_pagecache_writeback_confirm(pages, count, vn, index, gens,
                                   &g_dirty_pages, &g_dirty_page_count);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

static void page_cache_writeback_put(page_cache_page_t *page)
{
    page_cache_put(page);
}

static void page_cache_writeback_serial_lock(vnode_t *vn)
{
    mutex_t *lock = &g_page_cache_writeback_lock;
    if (vn)
        lock = &g_page_cache_writeback_locks[
            ((uintptr_t)vn >> 4) & (PAGE_CACHE_WRITEBACK_LOCKS - 1)];
    mutex_lock(lock);
}

static void page_cache_writeback_serial_unlock(vnode_t *vn)
{
    mutex_t *lock = &g_page_cache_writeback_lock;
    if (vn)
        lock = &g_page_cache_writeback_locks[
            ((uintptr_t)vn >> 4) & (PAGE_CACHE_WRITEBACK_LOCKS - 1)];
    mutex_unlock(lock);
}

static void page_cache_writeback_perf(size_t count)
{
    a20_perf_count(A20_PERF_PAGE_CACHE_WRITEBACK_BATCHES);
    a20_perf_add(A20_PERF_PAGE_CACHE_WRITEBACK_PAGES, count);
}

static int page_cache_writeback_common(vnode_t *vn,
                                       page_cache_writepage_t writepage,
                                       void *ctx, size_t max_pages,
                                       size_t *written_pages)
{
    if (!g_initialized)
        return 0;
    return a20_pagecache_writeback_transaction(
        vn, writepage, ctx, max_pages, written_pages,
        page_cache_writeback_collect, page_cache_writeback_lock_pages,
        page_cache_writeback_unlock_pages, page_cache_writeback_batch_allowed,
        page_cache_writeback_write, page_cache_writeback_alloc,
        page_cache_writeback_free, page_cache_writeback_confirm,
        page_cache_writeback_put, page_cache_writeback_serial_lock,
        page_cache_writeback_serial_unlock, page_cache_writeback_perf);
}

int page_cache_writeback_vnode(vnode_t *vn, page_cache_writepage_t writepage,
                               void *ctx)
{
    if (!vn)
        return -EINVAL;
    return page_cache_writeback_common(vn, writepage, ctx, 0, NULL);
}

int page_cache_writeback_all(page_cache_writepage_t writepage, void *ctx)
{
    return page_cache_writeback_common(NULL, writepage, ctx, 0, NULL);
}

static int page_cache_writeback_some(size_t max_pages)
{
    size_t written = 0;
    int r = page_cache_writeback_common(NULL, NULL, NULL, max_pages,
                                        &written);
    if (r < 0)
        return r;
    a20_perf_count(A20_PERF_PAGE_CACHE_PRESSURE_BATCHES);
    a20_perf_add(A20_PERF_PAGE_CACHE_PRESSURE_PAGES, written);
    return (int)written;
}

/* Keep normal buffered writes visible in the clean page cache after the
 * filesystem has committed them to its block cache.  This is deliberately a
 * write-through population path rather than writeback: compiler/linker output
 * is commonly reopened and executed immediately, and invalidating it forced a
 * second complete disk read.  Partial overwrites populate only when the old
 * bytes are already authoritative or the page lies wholly beyond old EOF. */
void page_cache_update_after_write(vnode_t *vn, uint64_t start,
                                   uint64_t old_size, const void *data,
                                   size_t len)
{
    if (!g_initialized || !vn || !data || len == 0)
        return;
    a20_pagecache_update_after_write(
        vn,start,old_size,data,len,page_cache_update_get,
        page_cache_file_ready,page_cache_file_lock_page,
        page_cache_file_unlock_page,page_cache_file_mark_uptodate,
        page_cache_file_mark_clean,page_cache_file_put,
        page_cache_file_write_perf);
}

void page_cache_invalidate(vnode_t *vn)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = a20_pagecache_detach_unpinned_range(
        vn, 0, ~(uint64_t)0, 1, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge, NULL);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_discard_unlinked(vnode_t *vn)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached;
    size_t discarded_dirty = 0;
    detached = a20_pagecache_detach_unpinned_range(
        vn, 0, ~(uint64_t)0, 0, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge,
        &discarded_dirty);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    a20_perf_add(A20_PERF_PAGE_CACHE_DISCARDED_DIRTY, discarded_dirty);
    /* The unlink path retains the transferred ext4 vnode-cache reference
     * until after its metadata lock is released, so dropping mapping-owned
     * references here cannot run ext4_release_vn() under that lock. */
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_invalidate_range(vnode_t *vn, uint64_t start_byte,
                                 uint64_t end_byte)
{
    if (!g_initialized || !vn || end_byte <= start_byte)
        return;
    uint64_t first_idx = start_byte / PAGE_SIZE;
    uint64_t last_idx  = (end_byte - 1) / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = a20_pagecache_detach_unpinned_range(
        vn, first_idx, last_idx, 0, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge, NULL);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_invalidate_uptodate_range(vnode_t *vn, uint64_t start_byte,
                                           uint64_t end_byte)
{
    if (!g_initialized || !vn || end_byte <= start_byte)
        return;
    uint64_t first_idx = start_byte / PAGE_SIZE;
    uint64_t last_idx  = (end_byte - 1) / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = a20_pagecache_detach_unpinned_range(
        vn, first_idx, last_idx, 0, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge, NULL);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_truncate(vnode_t *vn, uint64_t new_size)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = a20_pagecache_truncate(
        vn, new_size, page_cache_state_bucket_lock,
        page_cache_state_bucket_unlock, g_hash, &g_dirty_pages,
        &g_dirty_page_count, &g_valid_pages, page_cache_state_scan_bridge);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

size_t page_cache_drop_clean(void)
{
    if (!g_initialized)
        return 0;

    size_t dropped = 0;
    size_t visited = 0;
    int cursor = 0;
    while ((size_t)cursor < g_allocated_pages) {
        vnode_t *held[64];
        int held_count = 0;
        uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
        while ((size_t)cursor < g_allocated_pages && held_count < 64) {
            page_cache_page_t *page = page_cache_page_at((size_t)cursor++);
            visited++;
            vnode_t *vn = a20_pagecache_reclaim_clean(
                page, page_cache_state_bucket_lock, page_cache_state_bucket_unlock,
                page_cache_state_pfn_reclaimable, g_hash, &g_dirty_pages,
                &g_dirty_page_count, &g_valid_pages, &g_free_pages,
                page_cache_state_scan_bridge);
            if (vn) {
                held[held_count++] = vn;
                dropped++;
            }
        }
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        /* A final vnode_put() may enter the filesystem and acquire sleeping
         * locks.  Transfer the cache references out of the spinlocked region
         * before releasing them. */
        for (int i = 0; i < held_count; i++)
            vnode_put(held[i]);
    }
    page_cache_account_scan(visited);
    return dropped;
}

void page_cache_get_stats(page_cache_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    stats->capacity = g_page_limit;
    stats->bytes = g_page_limit * PAGE_SIZE;
    if (!g_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    stats->allocated = g_allocated_pages;
    stats->valid = g_valid_pages;
    stats->dirty = g_dirty_page_count;
    size_t pinned = 0;
    pinned = a20_pagecache_pinned_total((const size_t *)g_bucket_pinned);
    stats->pinned = pinned;
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

int page_cache_readahead(vfile_t *vf, uint64_t start_byte, size_t count)
{
    if (!vf || !vf->vnode)
        return -EINVAL;
    /* The admission rules and the EOF clamp are Rust-owned. */
    {
        size_t plan_count = 0;
        if (!a20_pagecache_readahead_plan(vf->vnode->type == VFS_FT_REGULAR,
                                          vf->vnode->size, start_byte, count,
                                          &plan_count))
            return 0;
        count = plan_count;
    }

    size_t done = 0;
    while (done < count) {
        uint64_t pos = start_byte + done;
        uint64_t index = pos / PAGE_SIZE;
        size_t page_off = (size_t)(pos % PAGE_SIZE);

        page_cache_page_t *page = page_cache_get(vf->vnode, index, 1);
        if (!page)
            break;
        if (!page_cache_is_uptodate(page)) {
            int r = page_cache_fill_vfile_page(vf, page);
            if (r < 0) {
                page_cache_put(page);
                break;
            }
        }
        page_cache_put(page);
        done += PAGE_SIZE - page_off;
    }
    return 0;
}

void page_cache_file_stats(vfile_t *vf, size_t *resident, size_t *dirty)
{
    if (resident)
        *resident = 0;
    if (dirty)
        *dirty = 0;
    if (!vf || !vf->vnode || !g_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    for (page_cache_page_t *page = vf->vnode->cache_pages; page;
         page = page->mapping_next) {
        if (resident)
            *resident += PAGE_SIZE;
        if (page->dirty && dirty)
            *dirty += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

void page_cache_file_range_stats(vfile_t *vf, uint64_t off_bytes,
                                 size_t len_bytes, size_t *resident,
                                 size_t *dirty)
{
    if (resident)
        *resident = 0;
    if (dirty)
        *dirty = 0;
    if (!vf || !vf->vnode || !g_initialized || len_bytes == 0)
        return;

    uint64_t first = off_bytes >> PAGE_SIZE_BITS;
    uint64_t last = (off_bytes + (len_bytes - 1)) >> PAGE_SIZE_BITS;
    if (last < first)
        last = ~(uint64_t)0 >> PAGE_SIZE_BITS;

    /* Counting the vnode's own mapping list keeps the cost proportional to the
     * queried file instead of the whole cache; the list is only ever spliced
     * under g_page_cache_lock. */
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    for (page_cache_page_t *page = vf->vnode->cache_pages; page;
         page = page->mapping_next) {
        if (page->index < first || page->index > last)
            continue;
        if (resident)
            *resident += PAGE_SIZE;
        if (page->dirty && dirty)
            *dirty += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}
