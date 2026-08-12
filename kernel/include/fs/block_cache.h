#ifndef _BLOCK_CACHE_H
#define _BLOCK_CACHE_H

#include "core/types.h"
#include "core/lock.h"
#include "core/sync.h"
#include "drivers/block/block_dev.h"

#define BCACHE_BLOCK_SIZE   512
#define BCACHE_MAX_BLOCKS   1024
#define BCACHE_HASH_BUCKETS 1024
#define PCACHE_PAGE_SIZE    4096
#define PCACHE_MAX_PAGES    2000
#define PCACHE_HASH_BUCKETS 512
#define PCACHE_FILL_LOCKS   64
#define BCACHE_READ_BATCH_MAX_PAGES 16

typedef struct bcache_entry {
    uint64_t    lba;
    int         dirty;
    uint64_t    dirty_gen;
    int         ref;
    int         valid;
    char        data[BCACHE_BLOCK_SIZE];
    struct bcache_entry *prev, *next;
    struct bcache_entry *hnext;
} bcache_entry_t;

typedef struct pcache_entry {
    uint64_t page_no;
    int      dirty;
    uint64_t dirty_gen;
    int      ref;
    int      valid;
    char     data[PCACHE_PAGE_SIZE];
    struct pcache_entry *prev, *next;
    struct pcache_entry *hnext;
} pcache_entry_t;

/* kmalloc's largest contiguous allocation is 8 MiB.  Keep the enlarged page
 * pool in the same buddy order as the old 1024-entry pool, with room for the
 * allocator header, so the optimization does not raise allocation pressure. */
_Static_assert(sizeof(pcache_entry_t) * PCACHE_MAX_PAGES + 64 <=
                   8U * 1024U * 1024U,
               "block page cache exceeds kmalloc maximum order");

typedef struct bcache {
    block_dev_t     *dev;
    bcache_entry_t  *pool;
    pcache_entry_t  *page_pool;
    char            *writeback_buffer;
    int              pool_size;
    int              page_pool_size;
    size_t           dirty_blocks;
    size_t           dirty_pages;
    spinlock_t       lock;
    /* Same-page fills serialize on one shard while unrelated cache misses
     * may issue block I/O concurrently. */
    mutex_t          fill_locks[PCACHE_FILL_LOCKS];
    rw_mutex_t       writeback_lock;
    /* When a device write fails (after the driver's own retry budget), further
     * flush attempts are suppressed until this tick deadline.  Dirty data
     * stays in cache so reads and in-memory writes keep working while the
     * device is wedged, and flush retries resume after the cooldown. */
    uint64_t         write_quarantine_until;
    bcache_entry_t   lru_head;
    bcache_entry_t   lru_tail;
    pcache_entry_t   page_lru_head;
    pcache_entry_t   page_lru_tail;
    bcache_entry_t  *hash[BCACHE_HASH_BUCKETS];
    pcache_entry_t  *page_hash[PCACHE_HASH_BUCKETS];
} bcache_t;

typedef struct bcache_stats {
    size_t caches;
    size_t block_pool_bytes;
    size_t page_pool_bytes;
    size_t valid_blocks;
    size_t dirty_blocks;
    size_t valid_pages;
    size_t dirty_pages;
} bcache_stats_t;

bcache_t *bcache_create(block_dev_t *dev);
void      bcache_destroy(bcache_t *bc);

bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba);
void      bcache_release(bcache_entry_t *e);
void      bcache_mark_dirty(bcache_entry_t *e);
int       bcache_sync_checked(bcache_t *bc);
void      bcache_sync(bcache_t *bc);
void      bcache_invalidate(bcache_t *bc, uint64_t lba);

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len);
int bcache_read_bytes_batch(bcache_t *bc, uint64_t byte_off, void *buf,
                            size_t len);
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len);
void bcache_get_stats(bcache_stats_t *stats);

#endif
