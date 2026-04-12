#ifndef _BLOCK_CACHE_H
#define _BLOCK_CACHE_H

#include "types.h"
#include "virtio_blk.h"

#define BCACHE_BLOCK_SIZE   512
#define BCACHE_MAX_BLOCKS   256

typedef struct bcache_entry {
    uint64_t    lba;
    int         dirty;
    int         ref;
    int         valid;
    char        data[BCACHE_BLOCK_SIZE];
    struct bcache_entry *prev, *next;
} bcache_entry_t;

typedef struct bcache {
    block_dev_t     *dev;
    bcache_entry_t  *pool;
    int              pool_size;
    bcache_entry_t   lru_head;
    bcache_entry_t   lru_tail;
} bcache_t;

bcache_t *bcache_create(block_dev_t *dev);
void      bcache_destroy(bcache_t *bc);

bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba);
void      bcache_release(bcache_entry_t *e);
void      bcache_mark_dirty(bcache_entry_t *e);
void      bcache_sync(bcache_t *bc);
void      bcache_invalidate(bcache_t *bc, uint64_t lba);

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len);
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len);

#endif
