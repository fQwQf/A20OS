#ifndef _BLOCK_CACHE_H
#define _BLOCK_CACHE_H

#include "types.h"
#include "virtio_blk.h"

/* ============================================================
 * Block Cache — LRU cache over block device sectors
 * Caches 512-byte sectors / 4096-byte clusters
 * Max 256 cached blocks at once
 * Inspired by RocketOS drivers/block/block_cache.rs
 * ============================================================ */

#define BCACHE_BLOCK_SIZE   512        /* virtio sector size */
#define BCACHE_MAX_BLOCKS   256        /* LRU cache capacity */

typedef struct bcache_entry {
    uint64_t    lba;           /* logical block address */
    int         dirty;
    int         ref;           /* reference count (pin) */
    int         valid;         /* data is valid */
    char        data[BCACHE_BLOCK_SIZE];
    struct bcache_entry *prev, *next;  /* LRU doubly-linked list */
} bcache_entry_t;

/* Initialize the block cache with a backing block device */
void bcache_init(block_dev_t *dev);

/* Get a block (reads from device if not cached).
 * Returns entry (ref++). Caller must call bcache_release(). */
bcache_entry_t *bcache_get(uint64_t lba);

/* Release a block (decrements ref). Dirty blocks stay cached. */
void bcache_release(bcache_entry_t *e);

/* Mark a block as dirty (will be written on eviction or sync). */
void bcache_mark_dirty(bcache_entry_t *e);

/* Write all dirty blocks back to device */
void bcache_sync(void);

/* Invalidate cached copy of a block (force re-read next access) */
void bcache_invalidate(uint64_t lba);

/* Read N bytes from byte offset on the block device.
 * Helper used by FAT32 driver. */
int bcache_read_bytes(uint64_t byte_off, void *buf, size_t len);

/* Write N bytes to byte offset on the block device. */
int bcache_write_bytes(uint64_t byte_off, const void *buf, size_t len);

#endif /* _BLOCK_CACHE_H */
