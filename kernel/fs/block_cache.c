#include "fs/block_cache.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"

static bcache_t *g_bcache_list[8];
static int g_bcache_count;

// 从 LRU 链表中移除一个条目
static void lru_remove(bcache_entry_t *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

// 将条目插入到 LRU 链表头部（表示最近使用）
static void lru_insert_front(bcache_t *bc, bcache_entry_t *e) {
    e->next             = bc->lru_head.next;
    e->prev             = &bc->lru_head;
    bc->lru_head.next->prev = e;
    bc->lru_head.next      = e;
}

static void page_lru_remove(pcache_entry_t *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

static void page_lru_insert_front(bcache_t *bc, pcache_entry_t *e) {
    e->next = bc->page_lru_head.next;
    e->prev = &bc->page_lru_head;
    bc->page_lru_head.next->prev = e;
    bc->page_lru_head.next = e;
}

static unsigned bcache_hash_key(uint64_t lba) {
    return (unsigned)((lba ^ (lba >> 32)) & (BCACHE_HASH_BUCKETS - 1));
}

static void bcache_hash_insert(bcache_t *bc, bcache_entry_t *e) {
    unsigned idx = bcache_hash_key(e->lba);
    e->hnext = bc->hash[idx];
    bc->hash[idx] = e;
}

static void bcache_hash_remove(bcache_t *bc, bcache_entry_t *e) {
    unsigned idx = bcache_hash_key(e->lba);
    bcache_entry_t **pp = &bc->hash[idx];
    while (*pp) {
        if (*pp == e) {
            *pp = e->hnext;
            e->hnext = NULL;
            return;
        }
        pp = &(*pp)->hnext;
    }
    e->hnext = NULL;
}

static unsigned pcache_hash_key(uint64_t page_no) {
    return (unsigned)((page_no ^ (page_no >> 32)) & (PCACHE_HASH_BUCKETS - 1));
}

static void pcache_hash_insert(bcache_t *bc, pcache_entry_t *e) {
    unsigned idx = pcache_hash_key(e->page_no);
    e->hnext = bc->page_hash[idx];
    bc->page_hash[idx] = e;
}

static void pcache_hash_remove(bcache_t *bc, pcache_entry_t *e) {
    unsigned idx = pcache_hash_key(e->page_no);
    pcache_entry_t **pp = &bc->page_hash[idx];
    while (*pp) {
        if (*pp == e) {
            *pp = e->hnext;
            e->hnext = NULL;
            return;
        }
        pp = &(*pp)->hnext;
    }
    e->hnext = NULL;
}

// 创建块缓存（分配 8192 个 512 字节的块）
bcache_t *bcache_create(block_dev_t *dev) {
    bcache_t *bc = (bcache_t *)kmalloc(sizeof(bcache_t));
    if (!bc) return NULL;
    memset(bc, 0, sizeof(*bc));

    bc->dev = dev;  // 绑定底层块设备
    bc->pool_size = BCACHE_MAX_BLOCKS;
    spin_init(&bc->lock);
    bc->pool = (bcache_entry_t *)kmalloc(sizeof(bcache_entry_t) * bc->pool_size);
    if (!bc->pool) { kfree(bc); return NULL; }
    bc->page_pool_size = PCACHE_MAX_PAGES;
    bc->page_pool = (pcache_entry_t *)kmalloc(sizeof(pcache_entry_t) * bc->page_pool_size);
    if (!bc->page_pool) { kfree(bc->pool); kfree(bc); return NULL; }

    // 初始化 LRU 链表（头尾哨兵节点）
    bc->lru_head.prev = NULL;
    bc->lru_head.next = &bc->lru_tail;
    bc->lru_tail.next = NULL;
    bc->lru_tail.prev = &bc->lru_head;
    bc->page_lru_head.prev = NULL;
    bc->page_lru_head.next = &bc->page_lru_tail;
    bc->page_lru_tail.next = NULL;
    bc->page_lru_tail.prev = &bc->page_lru_head;

    // 初始化所有缓存条目
    memset(bc->pool, 0, sizeof(bcache_entry_t) * bc->pool_size);
    for (int i = 0; i < bc->pool_size; i++) {
        bc->pool[i].valid = 0;
        bc->pool[i].dirty = 0;
        bc->pool[i].ref   = 0;
        bc->pool[i].lba   = (uint64_t)-1;
        lru_insert_front(bc, &bc->pool[i]);
    }
    memset(bc->page_pool, 0, sizeof(pcache_entry_t) * bc->page_pool_size);
    for (int i = 0; i < bc->page_pool_size; i++) {
        bc->page_pool[i].valid = 0;
        bc->page_pool[i].dirty = 0;
        bc->page_pool[i].ref = 0;
        bc->page_pool[i].page_no = (uint64_t)-1;
        page_lru_insert_front(bc, &bc->page_pool[i]);
    }

    printf("[BCACHE] Created cache: %d blocks + %d pages (%d KB)\n",
           bc->pool_size, bc->page_pool_size,
           (int)((bc->pool_size * BCACHE_BLOCK_SIZE +
                  bc->page_pool_size * PCACHE_PAGE_SIZE) / 1024));
    if (g_bcache_count < (int)(sizeof(g_bcache_list) / sizeof(g_bcache_list[0])))
        g_bcache_list[g_bcache_count++] = bc;
    return bc;
}

// 销毁块缓存（同步所有脏块并释放内存）
void bcache_destroy(bcache_t *bc) {
    if (!bc) return;
    bcache_sync(bc);  // 先同步所有脏块到磁盘
    for (int i = 0; i < g_bcache_count; i++) {
        if (g_bcache_list[i] == bc) {
            g_bcache_list[i] = g_bcache_list[g_bcache_count - 1];
            g_bcache_list[g_bcache_count - 1] = NULL;
            g_bcache_count--;
            break;
        }
    }
    if (bc->page_pool) kfree(bc->page_pool);
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
        stats->page_pool_bytes += (size_t)bc->page_pool_size * PCACHE_PAGE_SIZE;

        uint64_t flags = spin_lock_irqsave(&bc->lock);
        for (int j = 0; j < bc->pool_size; j++) {
            if (bc->pool[j].valid) {
                stats->valid_blocks++;
                if (bc->pool[j].dirty)
                    stats->dirty_blocks++;
            }
        }
        for (int j = 0; j < bc->page_pool_size; j++) {
            if (bc->page_pool[j].valid) {
                stats->valid_pages++;
                if (bc->page_pool[j].dirty)
                    stats->dirty_pages++;
            }
        }
        spin_unlock_irqrestore(&bc->lock, flags);
    }
}

static bcache_entry_t *bcache_find(bcache_t *bc, uint64_t lba) {
    for (bcache_entry_t *e = bc->hash[bcache_hash_key(lba)]; e; e = e->hnext) {
        if (e->valid && e->lba == lba)
            return e;
    }
    return NULL;
}

// 驱逐一个块（LRU 淘汰算法：从尾部找最久未使用的块）
static bcache_entry_t *bcache_evict(bcache_t *bc) {
    bcache_entry_t *e = bc->lru_tail.prev;  // 从最久未使用的开始
    while (e != &bc->lru_head) {
        if (e->ref == 0) {  // 只能驱逐引用计数为 0 的块
            lru_remove(e);
            if (e->valid)
                bcache_hash_remove(bc, e);
            e->ref = 1;
            return e;
        }
        e = e->prev;
    }
    return NULL;
}

// 获取一个块（从缓存或从磁盘读取）
bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba) {
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    bcache_entry_t *e = bcache_find(bc, lba);
    if (e) {
        // 命中缓存，增加引用计数并移到 LRU 头部
        e->ref++;
        lru_remove(e);
        lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        return e;
    }

    // 缓存未命中，驱逐一个旧块
    e = bcache_evict(bc);
    if (!e) {
        spin_unlock_irqrestore(&bc->lock, flags);
        printf("[BCACHE] no evictable block lba=%lu\n", (unsigned long)lba);
        return NULL;
    }
    uint64_t old_lba = e->lba;
    int old_dirty = e->dirty && e->valid;
    e->valid = 0;
    spin_unlock_irqrestore(&bc->lock, flags);

    if (old_dirty && bc->dev) {
        if (bc->dev->write_sector(bc->dev, old_lba, e->data, 1) < 0) {
            printf("[BCACHE] writeback error lba=%lu\n", (unsigned long)old_lba);
            flags = spin_lock_irqsave(&bc->lock);
            e->ref = 0;
            e->dirty = 1;
            e->valid = 1;
            bcache_hash_insert(bc, e);
            lru_insert_front(bc, e);
            spin_unlock_irqrestore(&bc->lock, flags);
            return NULL;
        }
    }

    // 从磁盘读取数据
    if (bc->dev) {
        int r = bc->dev->read_sector(bc->dev, lba, e->data, 1);
        if (r < 0) {
            printf("[BCACHE] read error lba=%lu\n", (unsigned long)lba);
            flags = spin_lock_irqsave(&bc->lock);
            e->ref = 0;
            e->lba = (uint64_t)-1;
            lru_insert_front(bc, e);
            spin_unlock_irqrestore(&bc->lock, flags);
            return NULL;
        }
    } else {
        // 没有底层设备，清零（用于内存文件系统）
        memset(e->data, 0, BCACHE_BLOCK_SIZE);
    }

    flags = spin_lock_irqsave(&bc->lock);
    /*
     * Race fix: another process may have fetched the same LBA while we
     * were doing I/O without the lock.  If so, discard our entry and
     * return the existing one to avoid duplicate hash entries.
     */
    bcache_entry_t *dup = bcache_find(bc, lba);
    if (dup) {
        e->ref = 0;
        e->lba = (uint64_t)-1;
        lru_insert_front(bc, e);
        dup->ref++;
        lru_remove(dup);
        lru_insert_front(bc, dup);
        spin_unlock_irqrestore(&bc->lock, flags);
        return dup;
    }
    e->lba = lba;
    e->dirty = 0;
    e->valid = 1;

    bcache_hash_insert(bc, e);
    lru_insert_front(bc, e);
    spin_unlock_irqrestore(&bc->lock, flags);
    return e;
}

// 释放块引用（减少引用计数）
void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    if (__atomic_load_n(&e->ref, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&e->ref, 1, __ATOMIC_RELEASE);
}

// 标记块为脏（数据已修改，需要写回磁盘）
void bcache_mark_dirty(bcache_entry_t *e) {
    if (e) __atomic_store_n(&e->dirty, 1, __ATOMIC_RELEASE);
}

// 同步所有脏块到磁盘（fsync 系统调用时使用）
void bcache_sync(bcache_t *bc) {
    if (!bc || !bc->dev) return;
    char tmp[BCACHE_BLOCK_SIZE];
    for (int i = 0; i < bc->page_pool_size; i++) {
        char page_tmp[PCACHE_PAGE_SIZE];
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        if (!bc->page_pool[i].valid || !bc->page_pool[i].dirty) {
            spin_unlock_irqrestore(&bc->lock, flags);
            continue;
        }
        uint64_t page_no = bc->page_pool[i].page_no;
        memcpy(page_tmp, bc->page_pool[i].data, PCACHE_PAGE_SIZE);
        spin_unlock_irqrestore(&bc->lock, flags);

        uint64_t lba = (page_no * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
        if (bc->dev->write_sector(bc->dev, lba, page_tmp,
                                  PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE) >= 0) {
            flags = spin_lock_irqsave(&bc->lock);
            if (bc->page_pool[i].valid && bc->page_pool[i].page_no == page_no)
                bc->page_pool[i].dirty = 0;
            spin_unlock_irqrestore(&bc->lock, flags);
        }
    }

    for (int i = 0; i < bc->pool_size; i++) {
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        if (!bc->pool[i].valid || !bc->pool[i].dirty) {
            spin_unlock_irqrestore(&bc->lock, flags);
            continue;
        }
        uint64_t lba = bc->pool[i].lba;
        memcpy(tmp, bc->pool[i].data, BCACHE_BLOCK_SIZE);
        spin_unlock_irqrestore(&bc->lock, flags);

        if (bc->dev->write_sector(bc->dev, lba, tmp, 1) >= 0) {
            flags = spin_lock_irqsave(&bc->lock);
            if (bc->pool[i].valid && bc->pool[i].lba == lba)
                bc->pool[i].dirty = 0;  // 写回成功，清除脏标志
            spin_unlock_irqrestore(&bc->lock, flags);
        }
    }
}

// 使缓存中的块失效（磁盘上的数据已改变）
void bcache_invalidate(bcache_t *bc, uint64_t lba) {
    if (!bc) return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    bcache_entry_t *e = bcache_find(bc, lba);
    if (e) {
        bcache_hash_remove(bc, e);
        e->valid = 0;
        e->dirty = 0;
    }
    spin_unlock_irqrestore(&bc->lock, flags);
}

static void bcache_invalidate_range(bcache_t *bc, uint64_t first_lba,
                                    uint64_t lba_count) {
    if (!bc || lba_count == 0)
        return;
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    for (uint64_t i = 0; i < lba_count; i++) {
        bcache_entry_t *e = bcache_find(bc, first_lba + i);
        if (e) {
            bcache_hash_remove(bc, e);
            e->valid = 0;
            e->dirty = 0;
        }
    }
    spin_unlock_irqrestore(&bc->lock, flags);
}

static pcache_entry_t *pcache_find(bcache_t *bc, uint64_t page_no) {
    for (pcache_entry_t *e = bc->page_hash[pcache_hash_key(page_no)]; e; e = e->hnext) {
        if (e->valid && e->page_no == page_no)
            return e;
    }
    return NULL;
}

static int pcache_flush_page(bcache_t *bc, pcache_entry_t *e) {
    if (!bc->dev || !e->dirty)
        return 0;
    uint64_t lba = (e->page_no * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
    int r = bc->dev->write_sector(bc->dev, lba, e->data,
                                  PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE);
    if (r == 0)
        e->dirty = 0;
    return r;
}

static pcache_entry_t *pcache_evict_locked(bcache_t *bc) {
    pcache_entry_t *e = bc->page_lru_tail.prev;
    while (e != &bc->page_lru_head) {
        if (e->ref == 0) {
            page_lru_remove(e);
            if (e->valid)
                pcache_hash_remove(bc, e);
            e->ref = 1;
            return e;
        }
        e = e->prev;
    }
    return NULL;
}

static pcache_entry_t *pcache_get(bcache_t *bc, uint64_t page_no, int skip_read) {
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    pcache_entry_t *e = pcache_find(bc, page_no);
    if (e) {
        e->ref++;
        page_lru_remove(e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        return e;
    }

    e = pcache_evict_locked(bc);
    if (!e) {
        spin_unlock_irqrestore(&bc->lock, flags);
        return NULL;
    }

    uint64_t old_page = e->page_no;
    int old_dirty = e->dirty && e->valid;
    e->valid = 0;
    spin_unlock_irqrestore(&bc->lock, flags);

    if (old_dirty && pcache_flush_page(bc, e) < 0) {
        flags = spin_lock_irqsave(&bc->lock);
        e->page_no = old_page;
        e->valid = 1;
        e->dirty = 1;
        e->ref = 0;
        pcache_hash_insert(bc, e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        return NULL;
    }

    if (skip_read) {
        /* Caller is about to overwrite the whole page. */
    } else if (bc->dev) {
        uint64_t lba = (page_no * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
        if (bc->dev->read_sector(bc->dev, lba, e->data,
                                 PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE) < 0) {
            flags = spin_lock_irqsave(&bc->lock);
            e->ref = 0;
            e->page_no = (uint64_t)-1;
            page_lru_insert_front(bc, e);
            spin_unlock_irqrestore(&bc->lock, flags);
            return NULL;
        }
    } else {
        memset(e->data, 0, PCACHE_PAGE_SIZE);
    }

    flags = spin_lock_irqsave(&bc->lock);
    /*
     * Race fix: another process may have fetched the same page while we
     * were doing I/O without the lock (sched() during virtio wait yields
     * to other processes).  If so, discard our entry and return the
     * existing one to avoid duplicate hash entries which corrupt the
     * hash chain and LRU list under sustained load.
     */
    pcache_entry_t *dup = pcache_find(bc, page_no);
    if (dup) {
        e->ref = 0;
        e->page_no = (uint64_t)-1;
        e->valid = 0;
        page_lru_insert_front(bc, e);
        dup->ref++;
        page_lru_remove(dup);
        page_lru_insert_front(bc, dup);
        spin_unlock_irqrestore(&bc->lock, flags);
        return dup;
    }

    e->page_no = page_no;
    e->dirty = 0;
    e->valid = 1;
    pcache_hash_insert(bc, e);
    page_lru_insert_front(bc, e);
    spin_unlock_irqrestore(&bc->lock, flags);
    return e;
}

static void pcache_release(pcache_entry_t *e) {
    if (!e) return;
    if (__atomic_load_n(&e->ref, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&e->ref, 1, __ATOMIC_RELEASE);
}

// 读取字节数据（可能跨多个块）
#define READAHEAD_PAGES 4

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len) {
    char *dst = (char *)buf;
    uint64_t first_page = byte_off / PCACHE_PAGE_SIZE;
    uint64_t last_page  = (byte_off + len - 1) / PCACHE_PAGE_SIZE;
    int sequential = (last_page - first_page + 1) >= 2;

    if (sequential) {
        uint64_t ra_end = last_page + READAHEAD_PAGES;
        for (uint64_t pn = last_page + 1; pn <= ra_end; pn++) {
            pcache_entry_t *e = pcache_get(bc, pn, 0);
            if (e) pcache_release(e);
        }
    }

    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t   off    = byte_off % PCACHE_PAGE_SIZE;
        size_t   chunk  = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;

        pcache_entry_t *e = pcache_get(bc, page_no, 0);
        if (!e) return -1;
        memcpy(dst, e->data + off, chunk);
        pcache_release(e);

        dst      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}

// 写入字节数据（可能跨多个块，标记块为脏）
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len) {
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t   off    = byte_off % PCACHE_PAGE_SIZE;
        size_t   chunk  = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;

        int full_page_overwrite = (off == 0 && chunk == PCACHE_PAGE_SIZE);
        pcache_entry_t *e = pcache_get(bc, page_no, full_page_overwrite);
        if (!e) return -1;
        memcpy(e->data + off, src, chunk);
        __atomic_store_n(&e->dirty, 1, __ATOMIC_RELEASE);
        bcache_invalidate_range(bc,
                                (page_no * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE,
                                PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE);
        pcache_release(e);

        src      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}
