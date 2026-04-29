#ifndef _BLOCK_CACHE_H
#define _BLOCK_CACHE_H

#include "core/types.h"
#include "core/lock.h"
#include "drv/virtio_blk.h"

#define BCACHE_BLOCK_SIZE   512
#define BCACHE_MAX_BLOCKS   512
#define BCACHE_HASH_BUCKETS 1024
#define PCACHE_PAGE_SIZE    4096
#define PCACHE_MAX_PAGES    128
#define PCACHE_HASH_BUCKETS 256

typedef struct bcache_entry {
    uint64_t    lba;
    int         dirty;
    int         ref;
    int         valid;
    char        data[BCACHE_BLOCK_SIZE];
    struct bcache_entry *prev, *next;
    struct bcache_entry *hnext;
} bcache_entry_t;

typedef struct pcache_entry {
    uint64_t page_no;
    int      dirty;
    int      ref;
    int      valid;
    char     data[PCACHE_PAGE_SIZE];
    struct pcache_entry *prev, *next;
    struct pcache_entry *hnext;
} pcache_entry_t;

typedef struct bcache {
    block_dev_t     *dev;
    bcache_entry_t  *pool;
    pcache_entry_t  *page_pool;
    int              pool_size;
    int              page_pool_size;
    spinlock_t       lock;
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
void      bcache_sync(bcache_t *bc);
void      bcache_invalidate(bcache_t *bc, uint64_t lba);

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len);
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len);
void bcache_get_stats(bcache_stats_t *stats);

#endif
