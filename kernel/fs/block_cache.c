/*
 * A20OS — Block Cache
 * LRU cache over a block_dev_t backing store.
 * Caches 512-byte sectors (BCACHE_BLOCK_SIZE).
 * Inspired by RocketOS block_cache.rs and xv6 bio.c.
 */

#include "block_cache.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

static block_dev_t   *g_dev = NULL;

/* Pool of cache entries */
static bcache_entry_t g_pool[BCACHE_MAX_BLOCKS];

/* LRU doubly-linked list: head = MRU, tail = LRU */
static bcache_entry_t g_lru_head;  /* sentinel */
static bcache_entry_t g_lru_tail;  /* sentinel */

static void lru_remove(bcache_entry_t *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

static void lru_insert_front(bcache_entry_t *e) {
    e->next             = g_lru_head.next;
    e->prev             = &g_lru_head;
    g_lru_head.next->prev = e;
    g_lru_head.next      = e;
}

void bcache_init(block_dev_t *dev) {
    g_dev = dev;

    /* Initialize LRU list */
    g_lru_head.prev = NULL;
    g_lru_head.next = &g_lru_tail;
    g_lru_tail.next = NULL;
    g_lru_tail.prev = &g_lru_head;

    /* Initialize pool */
    memset(g_pool, 0, sizeof(g_pool));
    for (int i = 0; i < BCACHE_MAX_BLOCKS; i++) {
        g_pool[i].valid = 0;
        g_pool[i].dirty = 0;
        g_pool[i].ref   = 0;
        g_pool[i].lba   = (uint64_t)-1;
        lru_insert_front(&g_pool[i]);
    }

    printf("[BCACHE] Initialized %d blocks (%d KB)\n",
           BCACHE_MAX_BLOCKS,
           (int)(BCACHE_MAX_BLOCKS * BCACHE_BLOCK_SIZE / 1024));
}

/* Find cached entry by LBA, or NULL */
static bcache_entry_t *bcache_find(uint64_t lba) {
    for (int i = 0; i < BCACHE_MAX_BLOCKS; i++) {
        if (g_pool[i].valid && g_pool[i].lba == lba) {
            return &g_pool[i];
        }
    }
    return NULL;
}

/* Evict the LRU unreferenced entry */
static bcache_entry_t *bcache_evict(void) {
    /* Walk from tail (LRU) toward head */
    bcache_entry_t *e = g_lru_tail.prev;
    while (e != &g_lru_head) {
        if (e->ref == 0) {
            /* Flush dirty data */
            if (e->dirty && g_dev && e->valid) {
                g_dev->write_sector(g_dev, e->lba, e->data, 1);
                e->dirty = 0;
            }
            lru_remove(e);
            e->valid = 0;
            e->lba   = (uint64_t)-1;
            return e;
        }
        e = e->prev;
    }
    panic("bcache_evict: all blocks are pinned!");
    return NULL;
}

bcache_entry_t *bcache_get(uint64_t lba) {
    bcache_entry_t *e = bcache_find(lba);
    if (e) {
        e->ref++;
        /* Move to front (MRU) */
        lru_remove(e);
        lru_insert_front(e);
        return e;
    }

    /* Miss — allocate a slot */
    e = bcache_evict();
    e->lba   = lba;
    e->dirty = 0;
    e->ref   = 1;
    e->valid = 0;

    /* Read from device */
    if (g_dev) {
        int r = g_dev->read_sector(g_dev, lba, e->data, 1);
        if (r < 0) {
            printf("[BCACHE] read error lba=%lu\n", (unsigned long)lba);
            e->ref = 0;
            return NULL;
        }
    } else {
        memset(e->data, 0, BCACHE_BLOCK_SIZE);
    }
    e->valid = 1;

    lru_insert_front(e);
    return e;
}

void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    if (e->ref > 0) e->ref--;
}

void bcache_mark_dirty(bcache_entry_t *e) {
    if (e) e->dirty = 1;
}

void bcache_sync(void) {
    if (!g_dev) return;
    for (int i = 0; i < BCACHE_MAX_BLOCKS; i++) {
        if (g_pool[i].valid && g_pool[i].dirty) {
            g_dev->write_sector(g_dev, g_pool[i].lba, g_pool[i].data, 1);
            g_pool[i].dirty = 0;
        }
    }
}

void bcache_invalidate(uint64_t lba) {
    for (int i = 0; i < BCACHE_MAX_BLOCKS; i++) {
        if (g_pool[i].lba == lba) {
            g_pool[i].valid = 0;
            g_pool[i].dirty = 0;
        }
    }
}

/* ---- Byte-level helpers for FAT32 ---- */

int bcache_read_bytes(uint64_t byte_off, void *buf, size_t len) {
    char *dst = (char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;
        if (chunk > len) chunk = len;

        bcache_entry_t *e = bcache_get(lba);
        if (!e) return -1;
        memcpy(dst, e->data + off, chunk);
        bcache_release(e);

        dst      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}

int bcache_write_bytes(uint64_t byte_off, const void *buf, size_t len) {
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;
        if (chunk > len) chunk = len;

        bcache_entry_t *e = bcache_get(lba);
        if (!e) return -1;
        memcpy(e->data + off, src, chunk);
        bcache_mark_dirty(e);
        bcache_release(e);

        src      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}
