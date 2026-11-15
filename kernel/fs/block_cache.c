#include "fs/block_cache.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/perf.h"
#include "core/errno.h"
#include "core/panic.h"
#include "core/timer.h"
#include "core/lock_counters.h"
#include "proc/proc.h"

/*
 * Block cache state engine selector.  Rust owns the cache's structural state
 * model (hash membership, LRU ordering, entry references, dirty accounting,
 * journal holds, quarantine, sync selection and the eviction policy).  The C
 * adapter keeps bc->lock, the bucket locks, the IO, the pools and the writeback
 * buffers.  The reference implementations below remain for BLOCK_CACHE_STATE_IMPL=c
 * and back the host differential harness.
 */
/* Provided by kernel/fs/block_cache_state.rs.  Every raw-helper entry point
 * requires the lock named in block_cache_state.rs's module doc. */
extern int a20_bcache_read_batch_plan(uint64_t byte_off, size_t len,
                                     uint64_t page_size, uint64_t block_size,
                                     size_t max_pages, uint64_t *out_first_page,
                                     size_t *out_page_count, uint64_t *out_lba,
                                     size_t *out_sectors);
extern unsigned a20_bcache_hash_key(uint64_t lba);
extern unsigned a20_pcache_hash_key(uint64_t page_no);
extern size_t a20_pcache_page_limit(size_t total_frames);
extern int a20_bcache_ref_read(const int *ref);
extern void a20_bcache_ref_get(int *ref);
extern void a20_bcache_ref_put(int *ref);
extern void a20_bcache_lru_insert_front(bcache_entry_t *head, bcache_entry_t *e);
extern void a20_bcache_lru_remove(bcache_entry_t *e);
extern void a20_pcache_lru_insert_front(pcache_entry_t *head, pcache_entry_t *e);
extern void a20_pcache_lru_remove(pcache_entry_t *e);
extern void a20_bcache_hash_insert_locked(bcache_entry_t **hash, bcache_entry_t *e);
extern void a20_bcache_hash_remove_locked(bcache_entry_t **hash, bcache_entry_t *e);
extern bcache_entry_t *a20_bcache_find_locked(bcache_entry_t **hash, uint64_t lba);
extern void a20_pcache_hash_remove_locked(pcache_entry_t **hash, pcache_entry_t *e);
extern pcache_entry_t *a20_pcache_find_locked(pcache_entry_t **hash, uint64_t page_no);
extern void a20_bcache_set_block_dirty(size_t *dirty_blocks, bcache_entry_t *e, int dirty);
extern void a20_bcache_set_page_dirty(pcache_dirty_set_t *dirty_pages,
                                      pcache_entry_t *e, int dirty);
extern int a20_bcache_hold_page(pcache_entry_t *pool, size_t n, uint64_t page_no);
extern void a20_bcache_release_holds(pcache_entry_t *pool, size_t n);
extern size_t a20_bcache_held_pages(const pcache_entry_t *pool, size_t n);
extern int a20_bcache_write_quarantined(const uint64_t *until, uint64_t now);
extern uint64_t a20_bcache_note_write_error(uint64_t *until, uint64_t now, uint64_t ticks);
extern int a20_bcache_sync_page_selected(const uint64_t *nos, size_t count, uint64_t page_no);
extern int a20_bcache_sync_select(int mode, int valid, int dirty, int held,
                                  const uint64_t *nos, size_t count, uint64_t page_no);
extern bcache_entry_t *a20_bcache_evict_transaction(
    bcache_entry_t *, bcache_entry_t *, bcache_entry_t **, void *, int,
    uint64_t (*)(void *, uint64_t), void (*)(void *, uint64_t, uint64_t));
extern pcache_entry_t *a20_pcache_evict_transaction(
    pcache_entry_t *, pcache_entry_t *, pcache_entry_t **, void *, int,
    uint64_t (*)(void *, uint64_t), void (*)(void *, uint64_t, uint64_t));
extern int a20_bcache_sync_transaction(
    void *, size_t, size_t, const uint64_t *, size_t, int,
    pcache_entry_t *, pcache_dirty_set_t *, bcache_entry_t *,
    uint64_t (*)(void *),
    void (*)(void *, uint64_t),
    void (*)(void *, size_t, uint64_t, uint64_t),
    void (*)(void *, const size_t *, const uint64_t *, const uint64_t *, size_t),
    int (*)(void *, uint64_t, const uint8_t *, size_t),
    int (*)(void *, const block_write_t *, size_t), int (*)(void *),
    void (*)(void *), void (*)(void *), void (*)(void *),
    uint8_t *(*)(void *, size_t), void (*)(void *, uint8_t *));
extern pcache_entry_t *a20_pcache_get_transaction(
    void *, pcache_entry_t *, size_t, pcache_entry_t **, pcache_entry_t *,
    pcache_entry_t *, pcache_dirty_set_t *, uint64_t, const uint8_t *, int,
    uint64_t (*)(void *, uint64_t), void (*)(void *, uint64_t, uint64_t),
    uint64_t (*)(void *), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t), void (*)(void *, uint64_t),
    int (*)(void *, pcache_entry_t *), int (*)(void *, uint64_t, uint8_t *),
    uint8_t *(*)(void *), void (*)(void *, int), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t));
extern void a20_pcache_release_storage(pcache_entry_t *, size_t, void (*)(uint8_t *));
extern bcache_entry_t *a20_bcache_get_transaction(
    void *, bcache_entry_t *, size_t, bcache_entry_t **, bcache_entry_t *,
    bcache_entry_t *, size_t *, uint64_t, int,
    uint64_t (*)(void *, uint64_t), void (*)(void *, uint64_t, uint64_t),
    uint64_t (*)(void *), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t), void (*)(void *, uint64_t),
    int (*)(void *, uint64_t, uint8_t *, int), void (*)(void *, uint64_t));
extern int a20_pcache_flush_transaction(
    void *, pcache_entry_t *, uint8_t *, pcache_dirty_set_t *, size_t,
    pcache_entry_t *(*)(void *, uint64_t), uint64_t (*)(void *),
    void (*)(void *, uint64_t), int (*)(void *, uint64_t, const uint8_t *, size_t));
extern int a20_bcache_read_batch_transaction(
    void *, uint64_t, uint8_t *, size_t, uint64_t, uint64_t, size_t,
    pcache_entry_t **, pcache_entry_t *, uint64_t (*)(void *),
    void (*)(void *, uint64_t), void (*)(void *, uint64_t),
    void (*)(void *, uint64_t), pcache_entry_t *(*)(void *, uint64_t),
    int (*)(void *, uint64_t, uint8_t *, size_t),
    int (*)(void *, uint64_t, uint8_t *, size_t), void (*)(void *, size_t));

static void pcache_free_page(uint8_t *data);

static bcache_t *g_bcache_list[8];
static int g_bcache_count;

#define BCACHE_SYNC_BATCH_PAGES 256

/* Charge the calling task for sectors actually transferred to/from the device.
 * Correct to attribute to proc_current(): A20OS has no flusher threads, so
 * writeback runs in the context of whoever triggered it. */
static void bcache_account_device_io(block_dev_t *dev, int sectors,
                                     int is_write)
{
    if (!dev || sectors <= 0)
        return;
    uint64_t bytes = (uint64_t)sectors * dev->sector_size;
    if (is_write)
        proc_io_account(0, 0, 0, bytes);
    else
        proc_io_account(0, 0, bytes, 0);
}

/* Cooldown after a failed device write before flush is attempted again. */
#define BCACHE_WRITE_QUARANTINE_TICKS (TICKS_PER_SEC * 30)

static int bcache_write_quarantined(bcache_t *bc) {
    return a20_bcache_write_quarantined(&bc->write_quarantine_until,
                                        timer_get_ticks());
}

static void bcache_note_write_error(bcache_t *bc) {
    a20_bcache_note_write_error(&bc->write_quarantine_until, timer_get_ticks(),
                                BCACHE_WRITE_QUARANTINE_TICKS);
}

/*
 * Cache entry references are acquired while bc->lock is held, but released
 * after the caller has finished copying data and therefore outside that
 * lock.  Both sides must still use atomic operations: a plain ref++ racing
 * with the final atomic decrement can lose the decrement and permanently pin
 * the entry.  A sustained parallel workload would eventually pin the whole
 * page pool and surface the resulting allocation failure as ext4 -EIO.
 */
static inline int cache_ref_read(const int *ref)
{
    return a20_bcache_ref_read(ref);
}

static inline void cache_ref_get(int *ref)
{
    a20_bcache_ref_get(ref);
}

static inline void cache_ref_put(int *ref)
{
    a20_bcache_ref_put(ref);
}

static void lru_insert_front(bcache_t *bc, bcache_entry_t *e) {
    a20_bcache_lru_insert_front(&bc->lru_head, e);
}

static void page_lru_insert_front(bcache_t *bc, pcache_entry_t *e) {
    a20_pcache_lru_insert_front(&bc->page_lru_head, e);
}

static unsigned bcache_hash_key(uint64_t lba) {
    return a20_bcache_hash_key(lba);
}

/* Bucket lock helpers.  Ordering rule: a caller may hold bc->lock and then a
 * bucket lock, but never acquire bc->lock while holding a bucket lock.  The
 * warm-hit path acquires only the bucket lock. */
static inline unsigned bcache_bucket_key(uint64_t lba)
{
    return bcache_hash_key(lba) & (BCACHE_BUCKET_LOCKS - 1);
}

static inline uint64_t bcache_bucket_lock_irqsave(bcache_t *bc, uint64_t lba)
{
    return spin_lock_irqsave(&bc->bucket_locks[bcache_bucket_key(lba)]);
}

static inline void bcache_bucket_unlock_irqrestore(bcache_t *bc, uint64_t lba,
                                                   uint64_t flags)
{
    spin_unlock_irqrestore(&bc->bucket_locks[bcache_bucket_key(lba)], flags);
}

/* Raw helpers: caller holds the entry's bucket lock. */
static void bcache_hash_remove_locked(bcache_t *bc, bcache_entry_t *e) {
    a20_bcache_hash_remove_locked(bc->hash, e);
}

static bcache_entry_t *bcache_find_locked(bcache_t *bc, uint64_t lba) {
    return a20_bcache_find_locked(bc->hash, lba);
}

/* Self-locking wrappers for callers that do not already hold the bucket lock
 * (used under bc->lock by paths that are not the warm-hit fast path). */
static void bcache_hash_remove(bcache_t *bc, bcache_entry_t *e) {
    uint64_t bf = bcache_bucket_lock_irqsave(bc, e->lba);
    bcache_hash_remove_locked(bc, e);
    bcache_bucket_unlock_irqrestore(bc, e->lba, bf);
}

static bcache_entry_t *bcache_find(bcache_t *bc, uint64_t lba) {
    uint64_t bf = bcache_bucket_lock_irqsave(bc, lba);
    bcache_entry_t *e = bcache_find_locked(bc, lba);
    bcache_bucket_unlock_irqrestore(bc, lba, bf);
    return e;
}

static unsigned pcache_hash_key(uint64_t page_no) {
    return a20_pcache_hash_key(page_no);
}

static inline unsigned pcache_bucket_key(uint64_t page_no)
{
    return pcache_hash_key(page_no) & (BCACHE_BUCKET_LOCKS - 1);
}

static inline uint64_t pcache_bucket_lock_irqsave(bcache_t *bc, uint64_t page_no)
{
    return spin_lock_irqsave(&bc->bucket_locks[pcache_bucket_key(page_no)]);
}

static inline void pcache_bucket_unlock_irqrestore(bcache_t *bc,
                                                   uint64_t page_no,
                                                   uint64_t flags)
{
    spin_unlock_irqrestore(&bc->bucket_locks[pcache_bucket_key(page_no)],
                           flags);
}


static void pcache_hash_remove_locked(bcache_t *bc, pcache_entry_t *e) {
    a20_pcache_hash_remove_locked(bc->page_hash, e);
}

static pcache_entry_t *pcache_find_locked(bcache_t *bc, uint64_t page_no) {
    return a20_pcache_find_locked(bc->page_hash, page_no);
}


static pcache_entry_t *pcache_find(bcache_t *bc, uint64_t page_no) {
    uint64_t bf = pcache_bucket_lock_irqsave(bc, page_no);
    pcache_entry_t *e = pcache_find_locked(bc, page_no);
    pcache_bucket_unlock_irqrestore(bc, page_no, bf);
    return e;
}

static void bcache_set_block_dirty_locked(bcache_t *bc, bcache_entry_t *e,
                                          int dirty)
{
    if (!bc || !e)
        return;
    a20_bcache_set_block_dirty(&bc->dirty_blocks, e, dirty);
}

static void bcache_set_page_dirty_locked(bcache_t *bc, pcache_entry_t *e,
                                         int dirty)
{
    if (!bc || !e)
        return;
    a20_bcache_set_page_dirty(&bc->dirty_pages, e, dirty);
}

/* ---- journal hold set -------------------------------------------------
 *
 * A filesystem that logs its metadata (ext4/JBD2) needs the block cache to
 * honour one rule: a page covered by an uncommitted transaction must not reach
 * its home location, whatever asks for a flush.  The hold bit lives on the
 * cache entry rather than in a side table so that both the writeback loop and
 * the evictor see it -- a held page that could be evicted would lose the only
 * copy of the new metadata, which is exactly what the transaction protects. */
void bcache_hold_page(bcache_t *bc, uint64_t page_no)
{
    if (!bc)
        return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    a20_bcache_hold_page(bc->page_pool, (size_t)bc->page_pool_size, page_no);
    spin_unlock_irqrestore(&bc->lock, flags);
}

void bcache_release_holds(bcache_t *bc)
{
    if (!bc)
        return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    a20_bcache_release_holds(bc->page_pool, (size_t)bc->page_pool_size);
    spin_unlock_irqrestore(&bc->lock, flags);
}

size_t bcache_held_pages(const bcache_t *bc)
{
    if (!bc)
        return 0;
    return a20_bcache_held_pages(bc->page_pool, (size_t)bc->page_pool_size);
}

void bcache_set_sync_hook(bcache_t *bc, int (*hook)(bcache_t *), void *owner)
{
    if (!bc)
        return;
    bc->sync_hook = hook;
    bc->owner = hook ? owner : NULL;
}

/* Pool is BCACHE_MAX_BLOCKS (1024) entries of BCACHE_BLOCK_SIZE (512) bytes,
 * i.e. 512 KiB.  Both constants live in kernel/include/fs/block_cache.h; a
 * previous version of this comment claimed 8192 entries, which contradicted
 * the header. */
bcache_t *bcache_create(block_dev_t *dev) {
    bcache_t *bc = (bcache_t *)kmalloc(sizeof(bcache_t));
    if (!bc) return NULL;
    memset(bc, 0, sizeof(*bc));

    bc->dev = dev;  // bind the underlying block device
    bc->pool_size = BCACHE_MAX_BLOCKS;
    spin_init(&bc->lock);
    lock_counters_register(&bc->lock, "block_cache");
    for (int i = 0; i < BCACHE_BUCKET_LOCKS; i++)
        spin_init(&bc->bucket_locks[i]);
    for (int i = 0; i < PCACHE_FILL_LOCKS; i++)
        mutex_init(&bc->fill_locks[i]);
    rw_mutex_init(&bc->writeback_lock);
    bc->pool = (bcache_entry_t *)kmalloc(sizeof(bcache_entry_t) * bc->pool_size);
    if (!bc->pool) { kfree(bc); return NULL; }
    bc->page_pool_size = (int)a20_pcache_page_limit((size_t)pfa.total_frames);
    bc->page_pool = (pcache_entry_t *)kmalloc(sizeof(pcache_entry_t) * bc->page_pool_size);
    if (!bc->page_pool) { kfree(bc->pool); kfree(bc); return NULL; }
    size_t dirty_word_count =
        ((size_t)bc->page_pool_size + 63U) / 64U;
    bc->dirty_pages.words = (uint64_t *)kmalloc(
        dirty_word_count * sizeof(*bc->dirty_pages.words));
    if (!bc->dirty_pages.words) {
        kfree(bc->page_pool);
        kfree(bc->pool);
        kfree(bc);
        return NULL;
    }
    bc->dirty_pages.count = 0;
    bc->dirty_pages.pool = bc->page_pool;
    bc->dirty_pages.pool_count = (size_t)bc->page_pool_size;
    memset(bc->dirty_pages.words, 0,
           dirty_word_count * sizeof(*bc->dirty_pages.words));
    bc->writeback_buffer =
        (char *)kmalloc(PCACHE_PAGE_SIZE * BCACHE_SYNC_BATCH_PAGES);
    if (!bc->writeback_buffer) {
        kfree(bc->dirty_pages.words);
        kfree(bc->page_pool);
        kfree(bc->pool);
        kfree(bc);
        return NULL;
    }

    // initialise the LRU lists (head and tail sentinel nodes)
    bc->lru_head.prev = NULL;
    bc->lru_head.next = &bc->lru_tail;
    bc->lru_tail.next = NULL;
    bc->lru_tail.prev = &bc->lru_head;
    bc->page_lru_head.prev = NULL;
    bc->page_lru_head.next = &bc->page_lru_tail;
    bc->page_lru_tail.next = NULL;
    bc->page_lru_tail.prev = &bc->page_lru_head;

    memset(bc->pool, 0, sizeof(bcache_entry_t) * bc->pool_size);
    for (int i = 0; i < bc->pool_size; i++) {
        bc->pool[i].valid = 0;
        bc->pool[i].dirty = 0;
        bc->pool[i].dirty_gen = 0;
        bc->pool[i].ref   = 0;
        bc->pool[i].accessed = 0;
        bc->pool[i].lba   = (uint64_t)-1;
        lru_insert_front(bc, &bc->pool[i]);
    }
    memset(bc->page_pool, 0, sizeof(pcache_entry_t) * bc->page_pool_size);
    for (int i = 0; i < bc->page_pool_size; i++) {
        bc->page_pool[i].valid = 0;
        bc->page_pool[i].dirty = 0;
        bc->page_pool[i].dirty_gen = 0;
        bc->page_pool[i].ref = 0;
        bc->page_pool[i].accessed = 0;
        bc->page_pool[i].page_no = (uint64_t)-1;
        page_lru_insert_front(bc, &bc->page_pool[i]);
    }

    kdebug("[BCACHE] Created cache: blocks=%d page_slots=%d payload_allocated_bytes=0\n",
           bc->pool_size, bc->page_pool_size);
    if (g_bcache_count < (int)(sizeof(g_bcache_list) / sizeof(g_bcache_list[0])))
        g_bcache_list[g_bcache_count++] = bc;
    return bc;
}

void bcache_destroy(bcache_t *bc) {
    if (!bc) return;
    bcache_sync(bc);
    for (int i = 0; i < g_bcache_count; i++) {
        if (g_bcache_list[i] == bc) {
            g_bcache_list[i] = g_bcache_list[g_bcache_count - 1];
            g_bcache_list[g_bcache_count - 1] = NULL;
            g_bcache_count--;
            break;
        }
    }
    if (bc->page_pool) {
        /* Destruction is exclusive: sync has completed and no caller, DMA, or
         * borrowed page pointer may outlive this cache. Only now detach/free
         * the slot-owned payload frames, before freeing their descriptors. */
        a20_pcache_release_storage(bc->page_pool, (size_t)bc->page_pool_size,
                                   pcache_free_page);
        kfree(bc->page_pool);
    }
    if (bc->dirty_pages.words)
        kfree(bc->dirty_pages.words);
    if (bc->writeback_buffer) kfree(bc->writeback_buffer);
    if (bc->pool) kfree(bc->pool);
    kfree(bc);
}

void bcache_get_stats(bcache_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    stats->caches = (size_t)g_bcache_count;
    for (int i = 0; i < g_bcache_count; i++) {
        bcache_t *bc = g_bcache_list[i];
        if (!bc)
            continue;
        stats->block_pool_bytes += (size_t)bc->pool_size * BCACHE_BLOCK_SIZE;
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        for (int j = 0; j < bc->pool_size; j++) {
            if (bc->pool[j].valid) {
                stats->valid_blocks++;
                if (bc->pool[j].dirty)
                    stats->dirty_blocks++;
            }
        }
        for (int j = 0; j < bc->page_pool_size; j++) {
            if (bc->page_pool[j].data)
                stats->page_pool_bytes += PCACHE_PAGE_SIZE;
            if (bc->page_pool[j].valid) {
                stats->valid_pages++;
                if (bc->page_pool[j].dirty)
                    stats->dirty_pages++;
            }
        }
        spin_unlock_irqrestore(&bc->lock, flags);
    }
}

// Evict one block (second-chance LRU: find the least recently used block from the tail; the accessed bit earns a reprieve once)
static uint64_t bcache_evict_bucket_lock(void *ctx, uint64_t key) { return bcache_bucket_lock_irqsave(ctx, key); }
static void bcache_evict_bucket_unlock(void *ctx, uint64_t key, uint64_t flags) { bcache_bucket_unlock_irqrestore(ctx, key, flags); }
static uint64_t bcache_get_global_lock(void *ctx) { return spin_lock_irqsave(&((bcache_t *)ctx)->lock); }
static void bcache_get_global_unlock(void *ctx,uint64_t f) { spin_unlock_irqrestore(&((bcache_t *)ctx)->lock,f); }
static void bcache_get_writeback_lock(void *ctx,uint64_t unused) { (void)unused; rw_mutex_write_lock(&((bcache_t *)ctx)->writeback_lock); }
static void bcache_get_writeback_unlock(void *ctx,uint64_t unused) { (void)unused; rw_mutex_write_unlock(&((bcache_t *)ctx)->writeback_lock); }
static int bcache_get_io(void *ctx,uint64_t lba,uint8_t *data,int write)
{
    bcache_t *bc=ctx;
    if(!bc->dev){if(!write)memset(data,0,BCACHE_BLOCK_SIZE);return 0;}
    int r=write?bc->dev->write_sector(bc->dev,lba,(char *)data,1):bc->dev->read_sector(bc->dev,lba,(char *)data,1);
    bcache_account_device_io(bc->dev,r,write); return r;
}
static void bcache_get_note_error(void *ctx,uint64_t lba) { (void)lba; bcache_note_write_error(ctx); }

bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba)
{
    if (!bc) return NULL;
    /* Keep the simple bucket-locked lookup/pin local. The Rust transaction
     * owns misses, publication and rollback; warm hits need no cross-language
     * transaction boundary. */
    uint64_t bf = bcache_bucket_lock_irqsave(bc, lba);
    unsigned idx = (unsigned)((lba ^ (lba >> 32)) &
                              (BCACHE_HASH_BUCKETS - 1));
    bcache_entry_t *hit = bc->hash[idx];
    while (hit && (!hit->valid || hit->lba != lba))
        hit = hit->hnext;
    if (hit) {
        cache_ref_get(&hit->ref);
        hit->accessed = 1;
        bcache_bucket_unlock_irqrestore(bc, lba, bf);
        return hit;
    }
    bcache_bucket_unlock_irqrestore(bc, lba, bf);
    return a20_bcache_get_transaction(
        bc,bc->pool,(size_t)bc->pool_size,bc->hash,&bc->lru_head,&bc->lru_tail,
        &bc->dirty_blocks,lba,bcache_write_quarantined(bc),
        bcache_evict_bucket_lock,bcache_evict_bucket_unlock,
        bcache_get_global_lock,bcache_get_global_unlock,
        bcache_get_writeback_lock,bcache_get_writeback_unlock,
        bcache_get_io,bcache_get_note_error);
}

void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    cache_ref_put(&e->ref);
}

void bcache_mark_dirty(bcache_entry_t *e) {
    if (!e) return;
    e->dirty_gen++;
    e->dirty = 1;
}

/*
 * Flush every dirty cache entry and report whether the storage device
 * accepted all writes.  Journal recovery must not claim success after a
 * timed-out VirtIO request merely because older callers used a void sync
 * interface.
 */
enum bcache_sync_mode {
    BC_SYNC_ALL,     /* every dirty page except journal-held ones */
    BC_SYNC_SCOPED,  /* the @page_nos selection, minus journal-held ones */
    BC_SYNC_HELD,    /* only journal-held pages (a transaction checkpoint) */
};

static uint64_t bcache_sync_cache_lock(void *ctx) { return spin_lock_irqsave(&((bcache_t *)ctx)->lock); }
static void bcache_sync_cache_unlock(void *ctx, uint64_t flags) { spin_unlock_irqrestore(&((bcache_t *)ctx)->lock,flags); }
static void bcache_sync_clear_pages(void *ctx, const size_t *indices,
                                    const uint64_t *nos, const uint64_t *gens,
                                    size_t count)
{
    bcache_t *bc=ctx; uint64_t f=spin_lock_irqsave(&bc->lock);
    for (size_t j=0;j<count;j++) {
        pcache_entry_t *e=&bc->page_pool[indices[j]];
        if (e->valid && e->page_no==nos[j] && e->dirty_gen==gens[j])
            bcache_set_page_dirty_locked(bc,e,0);
    }
    spin_unlock_irqrestore(&bc->lock,f);
}
static void bcache_sync_clear_block(void *ctx, size_t i, uint64_t lba, uint64_t gen)
{
    bcache_t *bc=ctx; uint64_t f=spin_lock_irqsave(&bc->lock); bcache_entry_t *e=&bc->pool[i];
    if (e->valid && e->lba==lba && e->dirty_gen==gen) bcache_set_block_dirty_locked(bc,e,0);
    spin_unlock_irqrestore(&bc->lock,f);
}
static int bcache_sync_write(void *ctx, uint64_t lba, const uint8_t *data, size_t sectors)
{
    bcache_t *bc=ctx; int r=bc->dev->write_sector(bc->dev,lba,(const char *)data,sectors);
    bcache_account_device_io(bc->dev,r,1);
    a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
    a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES,
                 sectors/(PCACHE_PAGE_SIZE/BCACHE_BLOCK_SIZE));
    return r;
}
static int bcache_sync_write_many(void *ctx, const block_write_t *vectors,
                                  size_t nr)
{
    bcache_t *bc = ctx;
    if (!bc || !bc->dev || !vectors || nr == 0)
        return -EINVAL;
    if (!bc->dev->write_batch) {
        for (size_t i = 0; i < nr; i++) {
            int r = bcache_sync_write(ctx, vectors[i].lba,
                                      vectors[i].buf, vectors[i].count);
            if (r < 0)
                return r;
        }
        return 0;
    }

    int r = bc->dev->write_batch(bc->dev, vectors, nr);
    /* Count submitted vectors as writeback attempts, including failed batches. */
    for (size_t i = 0; i < nr; i++) {
        a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
        a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES,
                     vectors[i].count / (PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE));
    }
    if (r == 0) {
        for (size_t i = 0; i < nr; i++)
            bcache_account_device_io(bc->dev, (int)vectors[i].count, 1);
    }
    return r;
}
static int bcache_sync_flush(void *ctx)
{
    bcache_t *bc=ctx; if (!bc->dev || !bc->dev->flush) return 0;
    a20_perf_count(A20_PERF_BLOCK_FLUSHES); return bc->dev->flush(bc->dev);
}
static void bcache_sync_lock(void *ctx) { rw_mutex_write_lock(&((bcache_t *)ctx)->writeback_lock); }
static void bcache_sync_unlock(void *ctx) { rw_mutex_write_unlock(&((bcache_t *)ctx)->writeback_lock); }
static void bcache_sync_error(void *ctx) { bcache_note_write_error(ctx); }
/* Sync borrows the cache-owned scratch page buffer while holding the exclusive
 * writeback lock. The same buffer serves eviction flushes and is freed only by
 * bcache_destroy(), after all users have stopped. */
static uint8_t *bcache_sync_borrow_buffer(void *ctx, size_t n)
{
    bcache_t *bc = ctx;
    const size_t capacity = PCACHE_PAGE_SIZE * BCACHE_SYNC_BATCH_PAGES;
    if (!bc || !bc->dev || !bc->writeback_buffer || n > capacity)
        return NULL;
    return (uint8_t *)bc->writeback_buffer;
}

static void bcache_sync_release_buffer(void *ctx, uint8_t *buffer)
{
    bcache_t *bc = ctx;
    if (!bc || !bc->writeback_buffer ||
        buffer != (uint8_t *)bc->writeback_buffer)
        panic("block cache: invalid writeback buffer release %p", buffer);
    /* This only ends the sync's borrow; the cache retains ownership. */
}

static int bcache_sync_common(bcache_t *bc, const uint64_t *page_nos,
                              size_t page_count, enum bcache_sync_mode mode)
{
    if (!bc || !bc->dev) return -EINVAL;
    if (bc->sync_hook && bc->sync_hook_depth == 0) {
        bc->sync_hook_depth++;
        int r=bc->sync_hook(bc);
        bc->sync_hook_depth--;
        if (r < 0) return r;
    }
    if (bcache_write_quarantined(bc)) return -EIO;
    return a20_bcache_sync_transaction(
        bc, (size_t)bc->page_pool_size, (size_t)bc->pool_size,
        page_nos, page_count, (int)mode, bc->page_pool, &bc->dirty_pages,
        bc->pool,
        bcache_sync_cache_lock, bcache_sync_cache_unlock,
        bcache_sync_clear_block, bcache_sync_clear_pages, bcache_sync_write,
        bcache_sync_write_many,
        bcache_sync_flush, bcache_sync_lock, bcache_sync_unlock,
        bcache_sync_error, bcache_sync_borrow_buffer,
        bcache_sync_release_buffer);
}

int bcache_sync_checked(bcache_t *bc) {
    return bcache_sync_common(bc, NULL, 0, BC_SYNC_ALL);
}

/*
 * fsync() scope: flush only the dirty 4 KiB pages whose page_no appears in
 * @page_nos (sorted ascending, deduplicated), plus any dirty 512-byte
 * metadata blocks.  Everything else dirty is left for its own fsync or a full
 * sync, so one file's fsync no longer writes the whole mount's block cache.
 * Ordering (writeback_lock + per-page dirty_gen) is identical to a full sync.
 */
int bcache_sync_scoped(bcache_t *bc, const uint64_t *page_nos, size_t count) {
    return bcache_sync_common(bc, page_nos, count, BC_SYNC_SCOPED);
}

/*
 * Journal checkpoint: flush only the pages a transaction is holding.  Called
 * by the filesystem immediately after its commit block is durable, at which
 * point the metadata may safely reach its home location -- so the caller
 * releases the holds (bcache_release_holds) only after this call succeeds.
 * Clearing them first would select no pages and lose the checkpoint.
 */
int bcache_sync_held(bcache_t *bc) {
    return bcache_sync_common(bc, NULL, 0, BC_SYNC_HELD);
}


// Exists for legacy fsync/unmount call sites that do not propagate errors upward.
void bcache_sync(bcache_t *bc) {
    (void)bcache_sync_checked(bc);
}

void bcache_invalidate(bcache_t *bc, uint64_t lba) {
    if (!bc) return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    bcache_entry_t *e = bcache_find(bc, lba);
    if (e) {
        bcache_hash_remove(bc, e);
        e->valid = 0;
        bcache_set_block_dirty_locked(bc, e, 0);
    }
    spin_unlock_irqrestore(&bc->lock, flags);
}

static uint64_t pcache_get_global_lock(void *ctx);
static void pcache_get_global_unlock(void *ctx, uint64_t flags);
static pcache_entry_t *pcache_find_adapter(void *ctx, uint64_t page_no)
{ return pcache_find(ctx, page_no); }
static int pcache_flush_write(void *ctx, uint64_t lba,
                              const uint8_t *data, size_t sectors);

static int pcache_flush_run(bcache_t *bc, pcache_entry_t *first)
{
    if (!bc->dev) return 0;
    return a20_pcache_flush_transaction(
        bc, first, (uint8_t *)bc->writeback_buffer, &bc->dirty_pages,
        BCACHE_SYNC_BATCH_PAGES, pcache_find_adapter,
        pcache_get_global_lock, pcache_get_global_unlock,
        pcache_flush_write);
}


static uint64_t pcache_get_bucket_lock(void *ctx,uint64_t page_no) { return pcache_bucket_lock_irqsave(ctx,page_no); }
static void pcache_get_bucket_unlock(void *ctx,uint64_t page_no,uint64_t f) { pcache_bucket_unlock_irqrestore(ctx,page_no,f); }
static uint64_t pcache_get_global_lock(void *ctx) { return spin_lock_irqsave(&((bcache_t *)ctx)->lock); }
static void pcache_get_global_unlock(void *ctx,uint64_t f) { spin_unlock_irqrestore(&((bcache_t *)ctx)->lock,f); }
static void pcache_get_fill_lock(void *ctx,uint64_t page_no)
{
 bcache_t *bc=ctx; mutex_t *lock=&bc->fill_locks[pcache_hash_key(page_no)&(PCACHE_FILL_LOCKS-1)];
 if(!mutex_trylock(lock)){a20_perf_count(A20_PERF_PCACHE_FILL_CONTENDED);mutex_lock(lock);}
}
static void pcache_get_fill_unlock(void *ctx,uint64_t page_no)
{bcache_t *bc=ctx;mutex_unlock(&bc->fill_locks[pcache_hash_key(page_no)&(PCACHE_FILL_LOCKS-1)]);}
static void pcache_get_writeback_lock(void *ctx,uint64_t unused){(void)unused;rw_mutex_write_lock(&((bcache_t *)ctx)->writeback_lock);}
static void pcache_get_writeback_unlock(void *ctx,uint64_t unused){(void)unused;rw_mutex_write_unlock(&((bcache_t *)ctx)->writeback_lock);}
static void pcache_failure(const char *stage, uint64_t page, int result)
{
    static unsigned reports;
    if (__atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 16)
        printf("[BCACHE] page fill failed stage=%s page=%llu rc=%d free_frames=%llu\n",
               stage, (unsigned long long)page, result,
               (unsigned long long)__atomic_load_n(&pfa.free_frames, __ATOMIC_RELAXED));
}
static int pcache_get_flush_victim(void *ctx, pcache_entry_t *e) {
    int r = pcache_flush_run(ctx,e);
    if (r < 0) pcache_failure("victim-write", e->page_no, r);
    return r;
}
static int pcache_get_read(void *ctx, uint64_t page_no, uint8_t *data)
{
    bcache_t *bc=ctx;
    if (!bc->dev) { memset(data,0,PCACHE_PAGE_SIZE); return 0; }
    uint64_t lba=page_no*PCACHE_PAGE_SIZE/BCACHE_BLOCK_SIZE;
    int r=bc->dev->read_sector(bc->dev,lba,(char *)data,PCACHE_PAGE_SIZE/BCACHE_BLOCK_SIZE);
    bcache_account_device_io(bc->dev,r,0);
    if (r < 0) pcache_failure("device-read", page_no, r);
    return r;
}
static void pcache_get_perf(void *ctx, int event) { (void)ctx; if(event==0) a20_perf_count(A20_PERF_PCACHE_FILL_MISSES); else if(event==3) a20_perf_count(A20_PERF_PCACHE_FULL_OVERWRITE_SKIPS); }
static void pcache_get_diagnostic(void *ctx, uint64_t page_no)
{
    bcache_t *bc=ctx; size_t valid=0,dirty=0,referenced=0; uint64_t total_refs=0; int max_refs=0;
    for(int i=0;i<bc->page_pool_size;i++){ pcache_entry_t *p=&bc->page_pool[i]; int refs=cache_ref_read(&p->ref); if(p->valid) valid++; if(p->valid&&p->dirty) dirty++; if(refs>0){referenced++;total_refs+=(uint64_t)refs;if(refs>max_refs)max_refs=refs;} }
    printf("[BCACHE] no evictable page page=%lu valid=%lu dirty=%lu referenced=%lu total_refs=%lu max_refs=%d\\n",(unsigned long)page_no,(unsigned long)valid,(unsigned long)dirty,(unsigned long)referenced,(unsigned long)total_refs,max_refs);
}
static void pcache_get_note_error(void *ctx, uint64_t unused) { (void)unused; bcache_note_write_error(ctx); }

/* Payload frames are order-0 allocations with reclaim disabled. This callback
 * runs with cache and writeback locks released; same-page fill serialization
 * remains held, and slots own the frame thereafter. */
static uint8_t *pcache_get_alloc_page(void *ctx)
{
    (void)ctx;
    pfn_t pfn = pfa_alloc_flags(0, 0);
    if (pfn == PFN_NONE) {
        pcache_failure("allocate-frame", 0, -ENOMEM);
        return NULL;
    }
    void *page = pfn_to_virt(pfn);
    if (!page) {
        pcache_failure("unmapped-frame", pfn, -EIO);
        frame_put(pfn);
        return NULL;
    }
    return (uint8_t *)page;
}

static void pcache_free_page(uint8_t *data)
{
    if (!data)
        return;
    pfn_t pfn = virt_to_pfn(data);
    if (pfn == PFN_NONE)
        panic("block cache: invalid owned page payload %p", data);
    frame_put(pfn);
}
static int pcache_flush_write(void *ctx, uint64_t lba,
                              const uint8_t *data, size_t sectors)
{
    bcache_t *bc = ctx;
    int r = bc->dev->write_sector(bc->dev, lba, (const char *)data, sectors);
    bcache_account_device_io(bc->dev, r, 1);
    a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
    a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES,
                 sectors / (PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE));
    return r;
}

static pcache_entry_t *pcache_get(bcache_t *bc, uint64_t page_no,
                                  const void *full_overwrite)
{
    if (!bc) return NULL;
    /* As with blocks, keep the simple warm hit local to C. Slow miss,
     * eviction, duplicate publication and rollback remain Rust-owned. */
    uint64_t bf = pcache_bucket_lock_irqsave(bc, page_no);
    unsigned idx = (unsigned)((page_no ^ (page_no >> 32)) &
                              (PCACHE_HASH_BUCKETS - 1));
    pcache_entry_t *hit = bc->page_hash[idx];
    while (hit && (!hit->valid || hit->page_no != page_no))
        hit = hit->hnext;
    if (hit) {
        cache_ref_get(&hit->ref);
        hit->accessed = 1;
        pcache_bucket_unlock_irqrestore(bc, page_no, bf);
        return hit;
    }
    pcache_bucket_unlock_irqrestore(bc, page_no, bf);
    return a20_pcache_get_transaction(
        bc, bc->page_pool, (size_t)bc->page_pool_size, bc->page_hash,
        &bc->page_lru_head, &bc->page_lru_tail, &bc->dirty_pages, page_no,
        full_overwrite, bcache_write_quarantined(bc),
        pcache_get_bucket_lock, pcache_get_bucket_unlock,
        pcache_get_global_lock, pcache_get_global_unlock,
        pcache_get_fill_lock, pcache_get_fill_unlock,
        pcache_get_writeback_lock, pcache_get_writeback_unlock,
        pcache_get_flush_victim, pcache_get_read, pcache_get_alloc_page,
        pcache_get_perf,
        pcache_get_diagnostic, pcache_get_note_error);
}

static void pcache_release(pcache_entry_t *e) {
    if (!e) return;
    cache_ref_put(&e->ref);
}

static void pcache_read_batch_wb_lock(void *ctx, uint64_t unused)
{ (void)unused; rw_mutex_read_lock(&((bcache_t *)ctx)->writeback_lock); }
static void pcache_read_batch_wb_unlock(void *ctx, uint64_t unused)
{ (void)unused; rw_mutex_read_unlock(&((bcache_t *)ctx)->writeback_lock); }
static pcache_entry_t *pcache_read_batch_find(void *ctx, uint64_t page_no)
{ return pcache_find(ctx, page_no); }
static int pcache_read_batch_io(void *ctx, uint64_t lba, uint8_t *buffer,
                                size_t sectors)
{
    bcache_t *bc = ctx;
    if (!bc->dev) { memset(buffer, 0, sectors * BCACHE_BLOCK_SIZE); return 0; }
    int r = bc->dev->read_sector(bc->dev, lba, (char *)buffer, sectors);
    bcache_account_device_io(bc->dev, r, 0);
    return r;
}
static int pcache_read_batch_fallback(void *ctx, uint64_t offset,
                                      uint8_t *buffer, size_t len)
{ return bcache_read_bytes(ctx, offset, buffer, len); }
static void pcache_read_batch_perf(void *ctx, size_t pages)
{ (void)ctx; a20_perf_add(A20_PERF_PCACHE_FILL_MISSES, pages); }

/* Rust owns batch eligibility and coherent overlay ordering. These callbacks
 * are the writeback lock, hash lookup, device read, and accounting edges. */
int bcache_read_bytes_batch(bcache_t *bc, uint64_t byte_off, void *buf,
                            size_t len) {
    if (!bc || !buf || len == 0)
        return len == 0 ? 0 : -EINVAL;
    return a20_bcache_read_batch_transaction(
        bc, byte_off, buf, len, PCACHE_PAGE_SIZE, BCACHE_BLOCK_SIZE,
        BCACHE_READ_BATCH_MAX_PAGES, bc->page_hash, &bc->page_lru_head,
        pcache_get_global_lock, pcache_get_global_unlock,
        pcache_read_batch_wb_lock, pcache_read_batch_wb_unlock,
        pcache_read_batch_find, pcache_read_batch_io,
        pcache_read_batch_fallback, pcache_read_batch_perf);
}

#define READAHEAD_PAGES 1

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len) {
    if (len == 0)
        return 0;
    char *dst = (char *)buf;
    uint64_t first_page = byte_off / PCACHE_PAGE_SIZE;
    uint64_t last_page = (byte_off + len - 1) / PCACHE_PAGE_SIZE;
    int sequential = (last_page - first_page + 1) >= 2;

    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t off = byte_off % PCACHE_PAGE_SIZE;
        size_t chunk = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;
        pcache_entry_t *e = pcache_get(bc, page_no, NULL);
        if (!e) return -1;
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        memcpy(dst, e->data + off, chunk);
        spin_unlock_irqrestore(&bc->lock, flags);
        pcache_release(e);
        dst += chunk; byte_off += chunk; len -= chunk;
    }
    if (sequential && READAHEAD_PAGES > 0) {
        uint64_t ra_end = last_page + READAHEAD_PAGES;
        for (uint64_t pn = last_page + 1; pn <= ra_end; pn++) {
            pcache_entry_t *e = pcache_get(bc, pn, NULL);
            if (e) pcache_release(e);
        }
    }
    return 0;
}

int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len) {
    if (!bc || len == 0)
        return 0;
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t off = byte_off % PCACHE_PAGE_SIZE;
        size_t chunk = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;
        const void *full_overwrite = (off == 0 && chunk == PCACHE_PAGE_SIZE) ? src : NULL;
        pcache_entry_t *e = pcache_get(bc, page_no, full_overwrite);
        if (!e) return -1;
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        memcpy(e->data + off, src, chunk);
        bcache_set_page_dirty_locked(bc, e, 1);
        spin_unlock_irqrestore(&bc->lock, flags);
        pcache_release(e);
        src += chunk; byte_off += chunk; len -= chunk;
    }
    return 0;
}

/*
 * Drop one 4 KiB page.  The journal writes its log blocks with synchronous
 * unbuffered I/O, in an order the cache cannot express (descriptor, then data,
 * then commit), so the cached image of a log block goes stale the moment it is
 * written.  Without this a later read would return the pre-write contents.
 * Only ever called on clean pages: a dirty one is dropped as well, because a
 * journal log block has no legitimate buffered copy.
 *
 * A reader that is already inside bcache_read_bytes() holds this entry and is
 * about to memcpy out of e->data.  Zeroing e->ref here would make the entry
 * look unreferenced to pcache_evict_locked(), which would hand it straight to
 * a fill for some OTHER page number -- and that fill overwrites e->data with
 * another file's bytes while the reader is still copying from it.  The reader
 * then returns a page of the wrong file, which is indistinguishable from disk
 * corruption downstream.  So the entry keeps its reference count and stays on
 * the LRU; the last holder's pcache_release() is what makes it evictable.
 */
void bcache_invalidate_page(bcache_t *bc, uint64_t page_no)
{
    if (!bc)
        return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    uint64_t bf = pcache_bucket_lock_irqsave(bc, page_no);
    pcache_entry_t *e = pcache_find_locked(bc, page_no);
    if (e) {
        pcache_hash_remove_locked(bc, e);
        e->valid = 0;
        e->held = 0;
        e->accessed = 0;
        bcache_set_page_dirty_locked(bc, e, 0);
        /* The entry stays on the LRU and keeps e->ref.  Off the hash and
         * valid == 0, so pcache_find_locked() can never hand it out again,
         * and pcache_evict_locked() skips it while the count is non-zero.
         * Once the last holder's pcache_release() drops it to zero it becomes
         * an ordinary invalid entry and is recycled like any other. */
    }
    pcache_bucket_unlock_irqrestore(bc, page_no, bf);
    spin_unlock_irqrestore(&bc->lock, flags);
}
