#include "block_cache.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

static void lru_remove(bcache_entry_t *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

static void lru_insert_front(bcache_t *bc, bcache_entry_t *e) {
    e->next             = bc->lru_head.next;
    e->prev             = &bc->lru_head;
    bc->lru_head.next->prev = e;
    bc->lru_head.next      = e;
}

bcache_t *bcache_create(block_dev_t *dev) {
    bcache_t *bc = (bcache_t *)kmalloc(sizeof(bcache_t));
    if (!bc) return NULL;
    memset(bc, 0, sizeof(*bc));

    bc->dev = dev;
    bc->pool_size = BCACHE_MAX_BLOCKS;
    bc->pool = (bcache_entry_t *)kmalloc(sizeof(bcache_entry_t) * bc->pool_size);
    if (!bc->pool) { kfree(bc); return NULL; }

    bc->lru_head.prev = NULL;
    bc->lru_head.next = &bc->lru_tail;
    bc->lru_tail.next = NULL;
    bc->lru_tail.prev = &bc->lru_head;

    memset(bc->pool, 0, sizeof(bcache_entry_t) * bc->pool_size);
    for (int i = 0; i < bc->pool_size; i++) {
        bc->pool[i].valid = 0;
        bc->pool[i].dirty = 0;
        bc->pool[i].ref   = 0;
        bc->pool[i].lba   = (uint64_t)-1;
        lru_insert_front(bc, &bc->pool[i]);
    }

    printf("[BCACHE] Created cache: %d blocks (%d KB)\n",
           bc->pool_size, (int)(bc->pool_size * BCACHE_BLOCK_SIZE / 1024));
    return bc;
}

void bcache_destroy(bcache_t *bc) {
    if (!bc) return;
    bcache_sync(bc);
    if (bc->pool) kfree(bc->pool);
    kfree(bc);
}

static bcache_entry_t *bcache_find(bcache_t *bc, uint64_t lba) {
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].valid && bc->pool[i].lba == lba) {
            return &bc->pool[i];
        }
    }
    return NULL;
}

static bcache_entry_t *bcache_evict(bcache_t *bc) {
    bcache_entry_t *e = bc->lru_tail.prev;
    while (e != &bc->lru_head) {
        if (e->ref == 0) {
            if (e->dirty && bc->dev && e->valid) {
                bc->dev->write_sector(bc->dev, e->lba, e->data, 1);
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

bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba) {
    bcache_entry_t *e = bcache_find(bc, lba);
    if (e) {
        e->ref++;
        lru_remove(e);
        lru_insert_front(bc, e);
        return e;
    }

    e = bcache_evict(bc);
    e->lba   = lba;
    e->dirty = 0;
    e->ref   = 1;
    e->valid = 0;

    if (bc->dev) {
        int r = bc->dev->read_sector(bc->dev, lba, e->data, 1);
        if (r < 0) {
            printf("[BCACHE] read error lba=%lu\n", (unsigned long)lba);
            e->ref = 0;
            return NULL;
        }
    } else {
        memset(e->data, 0, BCACHE_BLOCK_SIZE);
    }
    e->valid = 1;

    lru_insert_front(bc, e);
    return e;
}

void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    if (e->ref > 0) e->ref--;
}

void bcache_mark_dirty(bcache_entry_t *e) {
    if (e) e->dirty = 1;
}

void bcache_sync(bcache_t *bc) {
    if (!bc || !bc->dev) return;
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].valid && bc->pool[i].dirty) {
            bc->dev->write_sector(bc->dev, bc->pool[i].lba, bc->pool[i].data, 1);
            bc->pool[i].dirty = 0;
        }
    }
}

void bcache_invalidate(bcache_t *bc, uint64_t lba) {
    if (!bc) return;
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].lba == lba) {
            bc->pool[i].valid = 0;
            bc->pool[i].dirty = 0;
        }
    }
}

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len) {
    char *dst = (char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;
        if (chunk > len) chunk = len;

        bcache_entry_t *e = bcache_get(bc, lba);
        if (!e) return -1;
        memcpy(dst, e->data + off, chunk);
        bcache_release(e);

        dst      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}

int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len) {
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;
        if (chunk > len) chunk = len;

        bcache_entry_t *e = bcache_get(bc, lba);
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
