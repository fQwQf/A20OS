#include "fs/page_cache.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/string.h"
#include "core/lock_counters.h"

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
#define PAGE_CACHE_CHUNKS \
    (PAGE_CACHE_MAX_PAGES / PAGE_CACHE_CHUNK_PAGES)
static page_cache_page_t *g_page_chunks[PAGE_CACHE_CHUNKS];
static size_t g_allocated_pages;
static size_t g_page_limit;
static page_cache_page_t *g_free_pages;
static page_cache_page_t g_lru_head;
static page_cache_page_t g_lru_tail;
static page_cache_page_t *g_hash[PAGE_CACHE_HASH_BUCKETS];
static page_cache_page_t *g_dirty_pages;
static int g_initialized;

#define PAGE_CACHE_PRESSURE_WRITEBACK_PAGES 1024U

static int page_cache_writeback_some(size_t max_pages);

static inline void page_cache_account_scan(size_t entries)
{
    a20_perf_count(A20_PERF_PAGE_CACHE_SCAN_CALLS);
    a20_perf_add(A20_PERF_PAGE_CACHE_SCAN_ENTRIES, entries);
}

static unsigned page_cache_hash_key(vnode_t *vn, uint64_t index)
{
    uintptr_t v = vn && vn->mnt
        ? (uintptr_t)vn->mnt ^ (uintptr_t)vn->ino
        : (uintptr_t)vn;
    return (unsigned)((v >> 4) ^ index ^ (index >> 32)) &
           (PAGE_CACHE_HASH_BUCKETS - 1);
}

/* Lock ordering: a caller may hold g_page_cache_lock and then acquire a bucket
 * lock, but must never acquire g_page_cache_lock while holding a bucket lock.
 * The warm hit path acquires only the bucket lock. */
static inline unsigned page_cache_bucket(unsigned hash_idx)
{
    return hash_idx & (PAGE_CACHE_BUCKET_LOCKS - 1);
}

static inline uint64_t page_cache_bucket_lock_irqsave(unsigned hash_idx)
{
    return spin_lock_irqsave(&g_page_cache_bucket_locks[page_cache_bucket(hash_idx)]);
}

static inline void page_cache_bucket_unlock_irqrestore(unsigned hash_idx,
                                                       uint64_t flags)
{
    spin_unlock_irqrestore(&g_page_cache_bucket_locks[page_cache_bucket(hash_idx)],
                           flags);
}

static void lru_remove(page_cache_page_t *page)
{
    page->prev->next = page->next;
    page->next->prev = page->prev;
    page->prev = NULL;
    page->next = NULL;
}

static void lru_insert_front(page_cache_page_t *page)
{
    page->next = g_lru_head.next;
    page->prev = &g_lru_head;
    g_lru_head.next->prev = page;
    g_lru_head.next = page;
}

static void lru_insert_tail(page_cache_page_t *page)
{
    page->prev = g_lru_tail.prev;
    page->next = &g_lru_tail;
    g_lru_tail.prev->next = page;
    g_lru_tail.prev = page;
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
    page->hnext = g_free_pages;
    g_free_pages = page;
}

static page_cache_page_t *free_take_locked(void)
{
    page_cache_page_t *page = g_free_pages;
    if (!page)
        return NULL;
    g_free_pages = page->hnext;
    page->hnext = NULL;
    refcount_set(&page->ref_count, 1);
    lru_remove(page);
    lru_insert_front(page);
    return page;
}

/* The following hash helpers require the page's bucket lock to be held by the
 * caller (the same lock that guards the bucket chain and the refcount/valid
 * transition for a page found in it). */
static void hash_insert_locked(page_cache_page_t *page)
{
    unsigned idx = page_cache_hash_key(page->vnode, page->index);
    page->hnext = g_hash[idx];
    g_hash[idx] = page;
}

static void hash_remove_locked(page_cache_page_t *page)
{
    unsigned idx = page_cache_hash_key(page->vnode, page->index);
    page_cache_page_t **pp = &g_hash[idx];
    size_t visited = 0;
    while (*pp) {
        visited++;
        if (*pp == page) {
            *pp = page->hnext;
            page->hnext = NULL;
            page_cache_account_scan(visited);
            return;
        }
        pp = &(*pp)->hnext;
    }
    page_cache_account_scan(visited);
    page->hnext = NULL;
}

static page_cache_page_t *find_locked(vnode_t *vn, uint64_t index)
{
    size_t visited = 0;
    unsigned idx = page_cache_hash_key(vn, index);
    for (page_cache_page_t *p = g_hash[idx];
         p; p = p->hnext) {
        visited++;
        if (p->valid && p->vnode == vn && p->index == index) {
            page_cache_account_scan(visited);
            return p;
        }
    }
    page_cache_account_scan(visited);
    return NULL;
}

static void mapping_insert_locked(page_cache_page_t *page)
{
    vnode_t *vn = page->vnode;
    page->mapping_prev = NULL;
    page->mapping_next = vn->cache_pages;
    if (vn->cache_pages)
        vn->cache_pages->mapping_prev = page;
    vn->cache_pages = page;
}

static void mapping_remove_locked(page_cache_page_t *page)
{
    vnode_t *vn = page->vnode;
    if (page->mapping_prev)
        page->mapping_prev->mapping_next = page->mapping_next;
    else if (vn)
        vn->cache_pages = page->mapping_next;
    if (page->mapping_next)
        page->mapping_next->mapping_prev = page->mapping_prev;
    page->mapping_prev = NULL;
    page->mapping_next = NULL;
}

static void dirty_insert_locked(page_cache_page_t *page)
{
    if (page->dirty || !page->vnode)
        return;
    vnode_t *vn = page->vnode;
    /* Keep the per-vnode list in descending page-index order.  Sequential
     * append inserts at the head in O(1), while writeback consumes the tail
     * (lowest offset) so extent allocation sees monotonically increasing
     * logical blocks. */
    page_cache_page_t *head = vn->cache_dirty_pages;
    page_cache_page_t *tail = vn->cache_dirty_tail;
    if (!head) {
        page->dirty_prev = NULL;
        page->dirty_next = NULL;
        vn->cache_dirty_pages = page;
        vn->cache_dirty_tail = page;
    } else if (page->index >= head->index) {
        page->dirty_prev = NULL;
        page->dirty_next = head;
        head->dirty_prev = page;
        vn->cache_dirty_pages = page;
    } else if (page->index <= tail->index) {
        page->dirty_prev = tail;
        page->dirty_next = NULL;
        tail->dirty_next = page;
        vn->cache_dirty_tail = page;
    } else {
        page_cache_page_t *at = head;
        while (at->dirty_next && at->dirty_next->index > page->index)
            at = at->dirty_next;
        page->dirty_prev = at;
        page->dirty_next = at->dirty_next;
        at->dirty_next->dirty_prev = page;
        at->dirty_next = page;
    }

    page->global_dirty_prev = NULL;
    page->global_dirty_next = g_dirty_pages;
    if (g_dirty_pages)
        g_dirty_pages->global_dirty_prev = page;
    g_dirty_pages = page;
    page->dirty = 1;
}

static void dirty_remove_locked(page_cache_page_t *page)
{
    if (!page->dirty)
        return;
    vnode_t *vn = page->vnode;
    if (page->dirty_prev)
        page->dirty_prev->dirty_next = page->dirty_next;
    else if (vn)
        vn->cache_dirty_pages = page->dirty_next;
    if (page->dirty_next)
        page->dirty_next->dirty_prev = page->dirty_prev;
    else if (vn)
        vn->cache_dirty_tail = page->dirty_prev;

    if (page->global_dirty_prev)
        page->global_dirty_prev->global_dirty_next = page->global_dirty_next;
    else
        g_dirty_pages = page->global_dirty_next;
    if (page->global_dirty_next)
        page->global_dirty_next->global_dirty_prev = page->global_dirty_prev;

    page->dirty_prev = NULL;
    page->dirty_next = NULL;
    page->global_dirty_prev = NULL;
    page->global_dirty_next = NULL;
    page->dirty = 0;
}

/* Caller holds g_page_cache_lock AND the page's bucket lock.  Removing the
 * mapping pins and unlinks the page from hash, per-vnode mapping and dirty
 * lists in one critical section so a concurrent bucket-lock hit can neither
 * observe a half-detached page nor pin a page we are about to reclaim. */
static vnode_t *detach_mapping_deferred_locked(page_cache_page_t *page)
{
    if (!page->valid)
        return NULL;
    dirty_remove_locked(page);
    mapping_remove_locked(page);
    hash_remove_locked(page);
    vnode_t *vn = page->vnode;
    page->vnode = NULL;
    page->index = 0;
    page->valid = 0;
    page->dirty = 0;
    page->dirty_gen = 0;
    page->invalidate_gen++;
    page->uptodate = 0;
    return vn;
}

/*
 * Caller holds g_page_cache_lock.  Second-chance eviction: the global LRU
 * gives a rough insertion order; a candidate's bucket lock makes the
 * refcount/detach decision atomic against a concurrent warm hit, and the
 * accessed bit lets recently-used pages survive one eviction sweep without
 * any per-hit global LRU mutation.
 */
static page_cache_page_t *evict_locked(vnode_t **deferred_put)
{
    page_cache_page_t *page = g_lru_tail.prev;
    size_t visited = 0;
    while (page != &g_lru_head) {
        visited++;
        if (refcount_read(&page->ref_count) == 0 && !page->dirty &&
            pfn_valid(page->pfn) && pfa.meta[page->pfn].refcount <= 1) {
            if (page->valid) {
                unsigned idx = page_cache_hash_key(page->vnode, page->index);
                unsigned blk = page_cache_bucket(idx);
                uint64_t bflags = spin_lock_irqsave(&g_page_cache_bucket_locks[blk]);
                /* Re-check under the bucket lock: a warm hit may have pinned
                 * this page between the outer scan and here. */
                if (page->valid &&
                    refcount_read(&page->ref_count) == 0 && !page->dirty) {
                    if (page->accessed) {
                        page->accessed = 0;
                        page = page->prev;
                        spin_unlock_irqrestore(&g_page_cache_bucket_locks[blk], bflags);
                        continue;
                    }
                    *deferred_put = detach_mapping_deferred_locked(page);
                }
                spin_unlock_irqrestore(&g_page_cache_bucket_locks[blk], bflags);
                if (!page->valid) {
                    refcount_set(&page->ref_count, 1);
                    lru_remove(page);
                    lru_insert_front(page);
                    page_cache_account_scan(visited);
                    return page;
                }
                page = page->prev;
                continue;
            }
            /* A free descriptor: not in any hash chain, no bucket lock needed.
             * (When the free stack is empty no such page is on the LRU.) */
            refcount_set(&page->ref_count, 1);
            lru_remove(page);
            lru_insert_front(page);
            page_cache_account_scan(visited);
            return page;
        }
        page = page->prev;
    }
    page_cache_account_scan(visited);
    return NULL;
}

/* Allocate descriptors and frames in bounded chunks.  The old fixed cache
 * reserved 8 MiB at boot and thrashed on every large compiler image.  Lazy
 * chunks retain a large clean-file working set when memory is available while
 * leaving untouched systems at the old initial footprint. */
static int page_cache_grow(void)
{
    mutex_lock(&g_page_cache_grow_lock);
    uint64_t cache_flags = spin_lock_irqsave(&g_page_cache_lock);
    int growth_unneeded = g_allocated_pages >= g_page_limit ||
                          (g_initialized && g_free_pages != NULL);
    spin_unlock_irqrestore(&g_page_cache_lock, cache_flags);
    if (growth_unneeded) {
        mutex_unlock(&g_page_cache_grow_lock);
        return 0;
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

int page_cache_init(void)
{
    if (g_initialized)
        return 0;

    spin_init(&g_page_cache_lock);
    for (size_t i = 0; i < PAGE_CACHE_BUCKET_LOCKS; i++)
        spin_init(&g_page_cache_bucket_locks[i]);
    mutex_init(&g_page_cache_grow_lock);
    mutex_init(&g_page_cache_writeback_lock);
    for (size_t i = 0; i < PAGE_CACHE_WRITEBACK_LOCKS; i++)
        mutex_init(&g_page_cache_writeback_locks[i]);
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

page_cache_page_t *page_cache_get(vnode_t *vn, uint64_t index, int create)
{
    if (!g_initialized || !vn)
        return NULL;

    unsigned idx = page_cache_hash_key(vn, index);

    /* Warm hit fast path: only the bucket lock is taken.  No global LRU
     * mutation; the accessed bit feeds the second-chance evictor. */
    uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
    page_cache_page_t *page = find_locked(vn, index);
    if (page) {
        refcount_inc(&page->ref_count);
        page->accessed = 1;
        page_cache_bucket_unlock_irqrestore(idx, bflags);
        return page;
    }
    page_cache_bucket_unlock_irqrestore(idx, bflags);

    if (!create)
        return NULL;

retry:
    bflags = page_cache_bucket_lock_irqsave(idx);
    page = find_locked(vn, index);
    if (page) {
        refcount_inc(&page->ref_count);
        page->accessed = 1;
        page_cache_bucket_unlock_irqrestore(idx, bflags);
        return page;
    }
    page_cache_bucket_unlock_irqrestore(idx, bflags);

    vnode_t *deferred_put = NULL;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);

    /* Serialize the miss-to-create transition.  A mapping may have been
     * published after the optimistic bucket lookup but before this creator
     * acquired the global allocation lock. */
    bflags = page_cache_bucket_lock_irqsave(idx);
    page = find_locked(vn, index);
    if (page) {
        refcount_inc(&page->ref_count);
        page->accessed = 1;
        page_cache_bucket_unlock_irqrestore(idx, bflags);
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        return page;
    }
    page_cache_bucket_unlock_irqrestore(idx, bflags);

    page = free_take_locked();
    if (!page && g_allocated_pages < g_page_limit) {
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        if (page_cache_grow() == 0)
            goto retry;
        flags = spin_lock_irqsave(&g_page_cache_lock);
        bflags = page_cache_bucket_lock_irqsave(idx);
        page = find_locked(vn, index);
        if (page) {
            refcount_inc(&page->ref_count);
            page->accessed = 1;
            page_cache_bucket_unlock_irqrestore(idx, bflags);
            spin_unlock_irqrestore(&g_page_cache_lock, flags);
            return page;
        }
        page_cache_bucket_unlock_irqrestore(idx, bflags);
    }
    if (!page)
        page = evict_locked(&deferred_put);
    if (!page) {
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        /* Buffered writers are allowed to retain dirty data across close(),
         * so a large build can eventually consume every cache descriptor.
         * Make bounded forward progress under pressure: write a small batch,
         * then retry so evict_locked() can reclaim one of the newly clean
         * mappings.  fsync/sync remain the only explicit durability barriers;
         * this path is purely cache-pressure writeback. */
        int written = page_cache_writeback_some(
            PAGE_CACHE_PRESSURE_WRITEBACK_PAGES);
        if (written > 0)
            goto retry;
        return NULL;
    }
    page->vnode = vn;
    page->index = index;
    page->valid = 1;
    page->dirty = 0;
    page->dirty_gen = 0;
    page->invalidate_gen++;
    page->uptodate = 0;
    page->accessed = 0;
    memset(page->data, 0, PAGE_SIZE);
    vnode_get(vn);
    /* Global lock held: publish the mapping and the hash entry under the
     * bucket lock so warm hits observe fully-initialised fields. */
    bflags = page_cache_bucket_lock_irqsave(idx);
    hash_insert_locked(page);
    mapping_insert_locked(page);
    page_cache_bucket_unlock_irqrestore(idx, bflags);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    if (deferred_put)
        vnode_put(deferred_put);
    return page;
}

void page_cache_put(page_cache_page_t *page)
{
    if (!page)
        return;
    if (refcount_read(&page->ref_count) > 0)
        refcount_dec_and_test(&page->ref_count);
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

int page_cache_fill_vfile_page(vfile_t *vf, page_cache_page_t *page)
{
    if (!vf || !vf->ops || !vf->ops->read || !vf->ops->lseek)
        return -EINVAL;

    mutex_lock(&page->fill_lock);
    if (page_cache_is_uptodate(page)) {
        mutex_unlock(&page->fill_lock);
        return 0;
    }

    uint64_t page_base = page->index * PAGE_SIZE;
    void *data = page_cache_data(page);
    if (!data) {
        mutex_unlock(&page->fill_lock);
        return -ENOMEM;
    }

    uint64_t invalidate_gen;
retry:
    invalidate_gen = snapshot_invalidate_gen(page);
    if (vf->vnode->ops && vf->vnode->ops->readpage) {
        int r = vf->vnode->ops->readpage(vf->vnode, page->index,
                                         data, PAGE_SIZE);
        if (r < 0) {
            mutex_unlock(&page->fill_lock);
            return r;
        }
        if (!publish_uptodate(page, invalidate_gen))
            goto retry;
        mutex_unlock(&page->fill_lock);
        return r;
    }

    size_t saved = vf->offset;

    long seek_r = vf->ops->lseek(vf, (long)page_base, SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(&page->fill_lock);
        return (int)seek_r;
    }

    int r = vf->ops->read(vf, (char *)data, PAGE_SIZE);
    int restore_r = vf->ops->lseek(vf, (long)saved, SEEK_SET);
    if (restore_r < 0 && r >= 0)
        r = restore_r;
    if (r < 0) {
        mutex_unlock(&page->fill_lock);
        return r;
    }

    if ((size_t)r < PAGE_SIZE)
        memset((char *)data + r, 0, PAGE_SIZE - (size_t)r);
    if (!publish_uptodate(page, invalidate_gen))
        goto retry;
    mutex_unlock(&page->fill_lock);
    return r;
}

/* Fill an ascending, contiguous private-file fault window.  Filesystems with
 * readpages support receive a linear buffer and can merge physical disk I/O;
 * the data is scattered into the pinned cache pages only after the read. */
int page_cache_fill_vfile_pages(vfile_t *vf, page_cache_page_t **pages,
                                size_t count)
{
    if (!vf || !vf->vnode || !pages || count == 0 ||
        count > PAGE_CACHE_READAHEAD_PAGES)
        return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        if (!pages[i] || pages[i]->vnode != vf->vnode ||
            (i > 0 && pages[i]->index != pages[i - 1]->index + 1))
            return -EINVAL;
    }

    if (!vf->vnode->ops || !vf->vnode->ops->readpages) {
        for (size_t i = 0; i < count; i++) {
            if (!page_cache_is_uptodate(pages[i])) {
                int r = page_cache_fill_vfile_page(vf, pages[i]);
                if (r < 0)
                    return r;
            }
        }
        return 0;
    }

    char *buffer = (char *)kmalloc(count * PAGE_SIZE);
    if (!buffer) {
        /* Allocation pressure must not turn readahead into a fault failure. */
        return page_cache_fill_vfile_page(vf, pages[0]);
    }

    /* All callers build windows in increasing page-index order.  Taking the
     * per-page locks in that order prevents overlap deadlocks. */
    for (size_t i = 0; i < count; i++)
        mutex_lock(&pages[i]->fill_lock);

    int result = 0;
    for (;;) {
        int retry = 0;
        size_t cursor = 0;
        while (cursor < count) {
            while (cursor < count && page_cache_is_uptodate(pages[cursor]))
                cursor++;
            if (cursor == count)
                break;

            size_t start = cursor;
            while (cursor < count &&
                   !page_cache_is_uptodate(pages[cursor]))
                cursor++;
            size_t run = cursor - start;
            uint64_t generations[PAGE_CACHE_READAHEAD_PAGES];
            for (size_t i = 0; i < run; i++)
                generations[i] = snapshot_invalidate_gen(pages[start + i]);

            size_t bytes = run * PAGE_SIZE;
            memset(buffer, 0, bytes);
            int r = vf->vnode->ops->readpages(
                vf->vnode, pages[start]->index, buffer, bytes);
            if (r < 0) {
                result = r;
                goto out;
            }
            if ((size_t)r < bytes)
                memset(buffer + r, 0, bytes - (size_t)r);

            for (size_t i = 0; i < run; i++) {
                memcpy(page_cache_data(pages[start + i]),
                       buffer + i * PAGE_SIZE, PAGE_SIZE);
                if (!publish_uptodate(pages[start + i], generations[i]))
                    retry = 1;
            }
        }
        if (!retry)
            break;
    }

out:
    for (size_t i = count; i > 0; i--)
        mutex_unlock(&pages[i - 1]->fill_lock);
    kfree(buffer);
    return result;
}

/*
 * Ordinary read(2) used to fill one 4 KiB page at a time even when the
 * filesystem provided readpages().  Compiler inputs are predominantly
 * sequential and the ext4 implementation can merge a contiguous 128 KiB
 * window into one block request, so populate the forward window on the first
 * cold page.  The caller already pins pages[0]; pins acquired here are dropped
 * before returning and all publication remains protected by the existing
 * per-page fill locks.
 */
static int page_cache_readahead_vfile(vfile_t *vf,
                                      page_cache_page_t *first,
                                      size_t file_size)
{
    page_cache_page_t *pages[PAGE_CACHE_READAHEAD_PAGES];
    uint64_t file_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t count = 1;

    pages[0] = first;
    while (count < PAGE_CACHE_READAHEAD_PAGES &&
           first->index + count < file_pages) {
        pages[count] = page_cache_get(vf->vnode, first->index + count, 1);
        if (!pages[count])
            break;
        count++;
    }

    a20_perf_count(A20_PERF_PAGE_CACHE_READAHEAD_CALLS);
    a20_perf_add(A20_PERF_PAGE_CACHE_READAHEAD_PAGES, count);
    int result = page_cache_fill_vfile_pages(vf, pages, count);

    while (count > 1)
        page_cache_put(pages[--count]);
    return result;
}

pfn_t page_cache_pfn(page_cache_page_t *page)
{
    return page ? page->pfn : PFN_NONE;
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

    /*
     * Use the cached vnode->size directly.  File size is already updated by
     * write/truncate paths, so the expensive stat() call on every read was
     * a severe performance bottleneck for sequential I/O workloads.
     */
    size_t file_size = vf->vnode->size;
    size_t start = vf->offset;
    if (start >= file_size)
        return 0;
    if (count > file_size - start)
        count = file_size - start;

    size_t done = 0;
    while (done < count) {
        uint64_t pos = start + done;
        uint64_t index = pos / PAGE_SIZE;
        size_t page_off = (size_t)(pos % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > count - done)
            chunk = count - done;

        page_cache_page_t *page = page_cache_get(vf->vnode, index, 1);
        if (!page)
            break;

        if (!page_cache_is_uptodate(page)) {
            int r;
            if (vf->vnode->ops && vf->vnode->ops->readpages)
                r = page_cache_readahead_vfile(vf, page, file_size);
            else
                r = page_cache_fill_vfile_page(vf, page);
            if (r < 0) {
                page_cache_put(page);
                if (done == 0)
                    return r;
                break;
            }
        }

        memcpy(buf + done, (char *)page_cache_data(page) + page_off, chunk);
        page_cache_put(page);
        done += chunk;
    }

    if (done > 0) {
        long seek_r = vf->ops->lseek(vf, (long)(start + done), SEEK_SET);
        if (seek_r < 0)
            vf->offset = start + done;
    }
    return (int)done;
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

    size_t start = vf->offset;
    size_t old_size = vf->vnode->size;
    size_t done = 0;
    size_t updated = 0;
    while (done < count) {
        uint64_t pos = start + done;
        uint64_t index = pos / PAGE_SIZE;
        uint64_t page_start = index * PAGE_SIZE;
        size_t page_off = (size_t)(pos - page_start);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > count - done)
            chunk = count - done;

        page_cache_page_t *page = page_cache_get(vf->vnode, index, 1);
        if (!page)
            break;
        if (!page_cache_is_uptodate(page) &&
            page_start < old_size &&
            !(page_off == 0 && chunk == PAGE_SIZE)) {
            int r = page_cache_fill_vfile_page(vf, page);
            if (r < 0) {
                page_cache_put(page);
                if (done == 0)
                    return r;
                break;
            }
        }

        mutex_lock(&page->fill_lock);
        if (!page_cache_is_uptodate(page)) {
            memset(page_cache_data(page), 0, PAGE_SIZE);
            page_cache_mark_uptodate(page);
        }
        memcpy((char *)page_cache_data(page) + page_off,
               buf + done, chunk);
        page_cache_mark_dirty(page);
        mutex_unlock(&page->fill_lock);
        page_cache_put(page);
        done += chunk;
        updated++;
    }

    if (done != 0) {
        size_t end = start + done;
        if (end > vf->vnode->size)
            vf->vnode->size = end;
        long seek_r = vf->ops->lseek(vf, (long)end, SEEK_SET);
        if (seek_r < 0)
            vf->offset = end;
    }
    a20_perf_count(A20_PERF_PCACHE_WRITE_UPDATES);
    a20_perf_add(A20_PERF_PCACHE_WRITE_UPDATE_PAGES, updated);
    return (int)done;
}

static size_t collect_dirty_batch_locked(vnode_t *vn,
                                         page_cache_page_t **pages,
                                         size_t max_pages)
{
    page_cache_page_t *page = NULL;
    if (vn) {
        page = vn->cache_dirty_tail;
    } else if (g_dirty_pages && g_dirty_pages->vnode) {
        /* The global list is ordered by dirtied time, not file offset.  Pick
         * its vnode, then consume that vnode from the lowest dirty offset so
         * ext4 sees monotonically increasing logical blocks. */
        page = g_dirty_pages->vnode->cache_dirty_tail;
    }
    page_cache_account_scan(page ? 1 : 0);
    if (!page || max_pages == 0)
        return 0;

    vnode_t *batch_vn = page->vnode;
    uint64_t next_index = page->index;
    size_t count = 0;
    while (page && count < max_pages && page->vnode == batch_vn &&
           page->index == next_index) {
        refcount_inc(&page->ref_count);
        pages[count++] = page;
        next_index++;
        /* Per-vnode dirty pages are descending from head to tail, so the
         * predecessor of the tail is the next higher file offset. */
        page = page->dirty_prev;
    }
    return count;
}

static int page_cache_writeback_common(vnode_t *vn,
                                       page_cache_writepage_t writepage,
                                       void *ctx, size_t max_pages,
                                       size_t *written_pages)
{
    if (!g_initialized)
        return 0;

    page_cache_page_t *pages[PAGE_CACHE_WRITEBACK_BATCH_PAGES];
    uint64_t dirty_gens[PAGE_CACHE_WRITEBACK_BATCH_PAGES];
    void *linear = NULL;
    size_t written = 0;
    int result = 0;
    mutex_t *wb_lock = &g_page_cache_writeback_lock;
    if (vn)
        wb_lock = &g_page_cache_writeback_locks[
            ((uintptr_t)vn >> 4) & (PAGE_CACHE_WRITEBACK_LOCKS - 1)];
    mutex_lock(wb_lock);
    for (;;) {
        size_t batch_limit = PAGE_CACHE_WRITEBACK_BATCH_PAGES;
        if (max_pages != 0 && max_pages - written < batch_limit)
            batch_limit = max_pages - written;
        uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
        size_t count = collect_dirty_batch_locked(vn, pages, batch_limit);
        if (count == 0) {
            spin_unlock_irqrestore(&g_page_cache_lock, flags);
            break;
        }

        vnode_t *page_vn = pages[0]->vnode;
        uint64_t index = pages[0]->index;
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        /* A caller-supplied legacy callback describes one page only.  Native
         * vnode writepages may consume the whole bounded contiguous batch. */
        int use_batch = writepage == NULL && page_vn && page_vn->ops &&
                        page_vn->ops->writepages && count > 1;
        if (!use_batch) {
            for (size_t i = 1; i < count; i++)
                page_cache_put(pages[i]);
            count = 1;
        }

        for (size_t i = 0; i < count; i++)
            mutex_lock(&pages[i]->fill_lock);
        for (size_t i = 0; i < count; i++)
            dirty_gens[i] = __atomic_load_n(&pages[i]->dirty_gen,
                                             __ATOMIC_ACQUIRE);

        int r = 0;
        if (use_batch) {
            if (!linear)
                linear = kmalloc(PAGE_CACHE_WRITEBACK_BATCH_PAGES * PAGE_SIZE);
            if (!linear) {
                r = -ENOMEM;
            } else {
                for (size_t i = 0; i < count; i++)
                    memcpy((char *)linear + i * PAGE_SIZE,
                           pages[i]->data, PAGE_SIZE);
                r = page_vn->ops->writepages(page_vn, index, linear,
                                              count * PAGE_SIZE);
            }
        } else if (writepage) {
            r = writepage(page_vn, index, pages[0]->data, PAGE_SIZE, ctx);
        } else if (page_vn && page_vn->ops && page_vn->ops->writepage) {
            (void)ctx;
            r = page_vn->ops->writepage(page_vn, index, pages[0]->data,
                                         PAGE_SIZE);
        } else {
            r = -ENOSYS;
        }

        flags = spin_lock_irqsave(&g_page_cache_lock);
        /* Only clear dirty pages whose exact snapshot reached storage. */
        if (r >= 0) {
            for (size_t i = 0; i < count; i++) {
                page_cache_page_t *page = pages[i];
                if (page->valid && page->vnode == page_vn &&
                    page->index == index + i &&
                    __atomic_load_n(&page->dirty_gen, __ATOMIC_ACQUIRE) ==
                        dirty_gens[i])
                    dirty_remove_locked(page);
            }
        }
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        for (size_t i = count; i > 0; i--)
            mutex_unlock(&pages[i - 1]->fill_lock);
        for (size_t i = 0; i < count; i++)
            page_cache_put(pages[i]);
        if (r < 0) {
            result = r;
            break;
        }
        written += count;
        a20_perf_count(A20_PERF_PAGE_CACHE_WRITEBACK_BATCHES);
        a20_perf_add(A20_PERF_PAGE_CACHE_WRITEBACK_PAGES, count);
        if (written_pages)
            *written_pages = written;
        if (max_pages != 0 && written >= max_pages)
            break;
    }
    if (linear)
        kfree(linear);
    mutex_unlock(wb_lock);
    return result;
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

    const char *src = (const char *)data;
    size_t done = 0;
    size_t updated = 0;
    while (done < len) {
        uint64_t pos = start + done;
        uint64_t index = pos / PAGE_SIZE;
        uint64_t page_start = index * PAGE_SIZE;
        size_t page_off = (size_t)(pos - page_start);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len - done)
            chunk = len - done;
        int full_page = page_off == 0 && chunk == PAGE_SIZE;
        int beyond_old_eof = page_start >= old_size;

        page_cache_page_t *page = page_cache_get(vn, index, 0);
        if (!page && (full_page || beyond_old_eof))
            page = page_cache_get(vn, index, 1);
        if (page) {
            mutex_lock(&page->fill_lock);
            int authoritative = page_cache_is_uptodate(page);
            if (full_page || beyond_old_eof || authoritative) {
                if (!authoritative && !full_page)
                    memset(page_cache_data(page), 0, PAGE_SIZE);
                memcpy((char *)page_cache_data(page) + page_off,
                       src + done, chunk);
                page_cache_mark_uptodate(page);
                page_cache_mark_clean(page);
                updated++;
            }
            mutex_unlock(&page->fill_lock);
            page_cache_put(page);
        }
        done += chunk;
    }
    a20_perf_count(A20_PERF_PCACHE_WRITE_UPDATES);
    a20_perf_add(A20_PERF_PCACHE_WRITE_UPDATE_PAGES, updated);
}

void page_cache_invalidate(vnode_t *vn)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (refcount_read(&page->ref_count) == 0 && !page->dirty) {
            unsigned idx = page_cache_hash_key(page->vnode, page->index);
            uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
            if (page->valid && page->vnode == vn &&
                refcount_read(&page->ref_count) == 0 && !page->dirty) {
                detach_mapping_deferred_locked(page);
                free_insert_locked(page);
                detached++;
            }
            page_cache_bucket_unlock_irqrestore(idx, bflags);
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_discard_unlinked(vnode_t *vn)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t discarded_dirty = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (refcount_read(&page->ref_count) != 0)
            continue;
        if (page->dirty)
            discarded_dirty++;
        unsigned idx = page_cache_hash_key(page->vnode, page->index);
        uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
        if (page->valid && page->vnode == vn &&
            refcount_read(&page->ref_count) == 0) {
            detach_mapping_deferred_locked(page);
            free_insert_locked(page);
            detached++;
        }
        page_cache_bucket_unlock_irqrestore(idx, bflags);
    }
    page_cache_account_scan(visited);
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
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < first_idx || page->index > last_idx)
            continue;
        if (refcount_read(&page->ref_count) == 0) {
            unsigned idx = page_cache_hash_key(page->vnode, page->index);
            uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
            if (page->valid && page->vnode == vn &&
                page->index >= first_idx && page->index <= last_idx &&
                refcount_read(&page->ref_count) == 0) {
                detach_mapping_deferred_locked(page);
                free_insert_locked(page);
                detached++;
            }
            page_cache_bucket_unlock_irqrestore(idx, bflags);
        }
    }
    page_cache_account_scan(visited);
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
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < first_idx || page->index > last_idx)
            continue;
        if (refcount_read(&page->ref_count) == 0) {
            unsigned idx = page_cache_hash_key(page->vnode, page->index);
            uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
            if (page->valid && page->vnode == vn &&
                page->index >= first_idx && page->index <= last_idx &&
                refcount_read(&page->ref_count) == 0) {
                detach_mapping_deferred_locked(page);
                free_insert_locked(page);
                detached++;
            }
            page_cache_bucket_unlock_irqrestore(idx, bflags);
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_truncate(vnode_t *vn, uint64_t new_size)
{
    if (!g_initialized || !vn)
        return;
    uint64_t eof_index = new_size / PAGE_SIZE;
    size_t eof_offset = new_size % PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < eof_index)
            continue;
        int partial_eof_page = eof_offset && page->index == eof_index;
        if (partial_eof_page &&
            (page->dirty || page_cache_is_uptodate(page))) {
            /* Buffered writes may be newer than storage.  Retain the prefix
             * of the partial EOF page and discard only bytes beyond the new
             * size; detaching an unpinned dirty page here would lose data that
             * remains inside the truncated file. */
            memset((char *)page->data + eof_offset, 0,
                   PAGE_SIZE - eof_offset);
            page->invalidate_gen++;
            if (page->dirty)
                __atomic_add_fetch(&page->dirty_gen, 1, __ATOMIC_RELEASE);
        } else if (refcount_read(&page->ref_count) == 0) {
            unsigned idx = page_cache_hash_key(page->vnode, page->index);
            uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
            if (page->valid && page->vnode == vn &&
                page->index >= eof_index &&
                refcount_read(&page->ref_count) == 0) {
                detach_mapping_deferred_locked(page);
                free_insert_locked(page);
                detached++;
            }
            page_cache_bucket_unlock_irqrestore(idx, bflags);
        } else {
            /*
             * Page is pinned by a concurrent reader/writer.  We cannot
             * detach it, but we MUST invalidate its content so that if
             * the file is later extended, the stale old data is never
             * returned.  Zero the data and mark non-uptodate/non-dirty.
             */
            memset(page->data, 0, PAGE_SIZE);
            page->invalidate_gen++;
            __atomic_store_n(&page->uptodate, 0, __ATOMIC_RELEASE);
            dirty_remove_locked(page);
        }
    }
    page_cache_account_scan(visited);
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
            if (!page->valid || page->dirty ||
                refcount_read(&page->ref_count) != 0 ||
                !pfn_valid(page->pfn) ||
                pfa.meta[page->pfn].refcount > 1)
                continue;
            unsigned idx = page_cache_hash_key(page->vnode, page->index);
            uint64_t bflags = page_cache_bucket_lock_irqsave(idx);
            if (page->valid && !page->dirty &&
                refcount_read(&page->ref_count) == 0 &&
                pfn_valid(page->pfn) &&
                pfa.meta[page->pfn].refcount <= 1) {
                vnode_t *vn = detach_mapping_deferred_locked(page);
                if (vn) {
                    free_insert_locked(page);
                    held[held_count++] = vn;
                }
                dropped++;
            }
            page_cache_bucket_unlock_irqrestore(idx, bflags);
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
    for (size_t i = 0; i < g_allocated_pages; i++) {
        page_cache_page_t *page = page_cache_page_at(i);
        if (!page->valid)
            continue;
        stats->valid++;
        if (page->dirty)
            stats->dirty++;
        if (refcount_read(&page->ref_count) > 0)
            stats->pinned++;
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

int page_cache_readahead(vfile_t *vf, uint64_t start_byte, size_t count)
{
    if (!vf || !vf->vnode)
        return -EINVAL;
    if (vf->vnode->type != VFS_FT_REGULAR)
        return 0;
    if (count == 0)
        return 0;

    size_t file_size = vf->vnode->size;
    if (start_byte >= file_size)
        return 0;
    if (count > file_size - start_byte)
        count = file_size - start_byte;

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
    for (size_t i = 0; i < g_allocated_pages; i++) {
        page_cache_page_t *page = page_cache_page_at(i);
        if (!page)
            continue;
        if (!page->valid || page->vnode != vf->vnode)
            continue;
        if (resident)
            *resident += PAGE_SIZE;
        if (page->dirty && dirty)
            *dirty += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}
