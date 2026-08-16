#include "fs/block_cache.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/perf.h"
#include "core/errno.h"
#include "core/timer.h"
#include "core/lock_counters.h"

static bcache_t *g_bcache_list[8];
static int g_bcache_count;

#define BCACHE_SYNC_BATCH_PAGES 256

/* Cooldown after a failed device write before flush is attempted again. */
#define BCACHE_WRITE_QUARANTINE_TICKS (TICKS_PER_SEC * 30)

static int bcache_write_quarantined(bcache_t *bc) {
    return bc->write_quarantine_until &&
           timer_get_ticks() < bc->write_quarantine_until;
}

static void bcache_note_write_error(bcache_t *bc) {
    bc->write_quarantine_until =
        timer_get_ticks() + BCACHE_WRITE_QUARANTINE_TICKS;
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
    return __atomic_load_n(ref, __ATOMIC_ACQUIRE);
}

static inline void cache_ref_get(int *ref)
{
    __atomic_fetch_add(ref, 1, __ATOMIC_ACQUIRE);
}

static inline void cache_ref_put(int *ref)
{
    int current = __atomic_load_n(ref, __ATOMIC_ACQUIRE);
    while (current > 0) {
        if (__atomic_compare_exchange_n(ref, &current, current - 1, 0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_ACQUIRE))
            return;
    }
}

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
static void bcache_hash_insert_locked(bcache_t *bc, bcache_entry_t *e) {
    unsigned idx = bcache_hash_key(e->lba);
    e->hnext = bc->hash[idx];
    bc->hash[idx] = e;
}

static void bcache_hash_remove_locked(bcache_t *bc, bcache_entry_t *e) {
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

static bcache_entry_t *bcache_find_locked(bcache_t *bc, uint64_t lba) {
    for (bcache_entry_t *e = bc->hash[bcache_hash_key(lba)]; e; e = e->hnext) {
        if (e->valid && e->lba == lba)
            return e;
    }
    return NULL;
}

/* Self-locking wrappers for callers that do not already hold the bucket lock
 * (used under bc->lock by paths that are not the warm-hit fast path). */
static void bcache_hash_insert(bcache_t *bc, bcache_entry_t *e) {
    uint64_t bf = bcache_bucket_lock_irqsave(bc, e->lba);
    bcache_hash_insert_locked(bc, e);
    bcache_bucket_unlock_irqrestore(bc, e->lba, bf);
}

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
    return (unsigned)((page_no ^ (page_no >> 32)) & (PCACHE_HASH_BUCKETS - 1));
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

static void pcache_hash_insert_locked(bcache_t *bc, pcache_entry_t *e) {
    unsigned idx = pcache_hash_key(e->page_no);
    e->hnext = bc->page_hash[idx];
    bc->page_hash[idx] = e;
}

static void pcache_hash_remove_locked(bcache_t *bc, pcache_entry_t *e) {
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

static pcache_entry_t *pcache_find_locked(bcache_t *bc, uint64_t page_no) {
    for (pcache_entry_t *e = bc->page_hash[pcache_hash_key(page_no)];
         e; e = e->hnext) {
        if (e->valid && e->page_no == page_no)
            return e;
    }
    return NULL;
}

static void pcache_hash_insert(bcache_t *bc, pcache_entry_t *e) {
    uint64_t bf = pcache_bucket_lock_irqsave(bc, e->page_no);
    pcache_hash_insert_locked(bc, e);
    pcache_bucket_unlock_irqrestore(bc, e->page_no, bf);
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
    if (dirty) {
        e->dirty_gen++;
        if (!e->dirty)
            bc->dirty_blocks++;
        e->dirty = 1;
    } else if (e->dirty) {
        e->dirty = 0;
        if (bc->dirty_blocks > 0)
            bc->dirty_blocks--;
    }
}

static void bcache_set_page_dirty_locked(bcache_t *bc, pcache_entry_t *e,
                                         int dirty)
{
    if (!bc || !e)
        return;
    if (dirty) {
        e->dirty_gen++;
        if (!e->dirty)
            bc->dirty_pages++;
        e->dirty = 1;
    } else if (e->dirty) {
        e->dirty = 0;
        if (bc->dirty_pages > 0)
            bc->dirty_pages--;
    }
}

// 创建块缓存（分配 8192 个 512 字节的块）
bcache_t *bcache_create(block_dev_t *dev) {
    bcache_t *bc = (bcache_t *)kmalloc(sizeof(bcache_t));
    if (!bc) return NULL;
    memset(bc, 0, sizeof(*bc));

    bc->dev = dev;  // 绑定底层块设备
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
    bc->page_pool_size = PCACHE_MAX_PAGES;
    bc->page_pool = (pcache_entry_t *)kmalloc(sizeof(pcache_entry_t) * bc->page_pool_size);
    if (!bc->page_pool) { kfree(bc->pool); kfree(bc); return NULL; }
    bc->writeback_buffer =
        (char *)kmalloc(PCACHE_PAGE_SIZE * BCACHE_SYNC_BATCH_PAGES);
    if (!bc->writeback_buffer) {
        kfree(bc->page_pool);
        kfree(bc->pool);
        kfree(bc);
        return NULL;
    }

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

    kdebug("[BCACHE] Created cache: %d blocks + %d pages (%d KB)\n",
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

// 驱逐一个块（第二机会 LRU：从尾部找最久未使用的块，accessed 位可留一次）
static bcache_entry_t *bcache_evict(bcache_t *bc) {
    int quarantined = bcache_write_quarantined(bc);
    bcache_entry_t *e = bc->lru_tail.prev;  // 从最久未使用的开始
    while (e != &bc->lru_head) {
        if (cache_ref_read(&e->ref) == 0) {  // 只能驱逐引用计数为 0 的块
            /* While the device is known-wedged, prefer clean victims: a dirty
             * victim would force a flush that can only time out again. */
            if (quarantined && e->valid && e->dirty) {
                e = e->prev;
                continue;
            }
            if (e->valid) {
                uint64_t bf = bcache_bucket_lock_irqsave(bc, e->lba);
                if (cache_ref_read(&e->ref) == 0 && e->valid &&
                    !(quarantined && e->dirty)) {
                    if (e->accessed) {
                        e->accessed = 0;
                        bcache_bucket_unlock_irqrestore(bc, e->lba, bf);
                        e = e->prev;
                        continue;
                    }
                    lru_remove(e);
                    bcache_hash_remove_locked(bc, e);
                    e->ref = 1;
                    bcache_bucket_unlock_irqrestore(bc, e->lba, bf);
                    return e;
                }
                bcache_bucket_unlock_irqrestore(bc, e->lba, bf);
                e = e->prev;
                continue;
            }
            lru_remove(e);
            e->ref = 1;
            return e;
        }
        e = e->prev;
    }
    return NULL;
}

// 获取一个块（从缓存或从磁盘读取）
bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba) {
    /* Warm hit fast path: only the bucket lock is taken; the accessed bit
     * feeds the second-chance evictor instead of a per-hit global LRU move. */
    uint64_t bf = bcache_bucket_lock_irqsave(bc, lba);
    bcache_entry_t *e = bcache_find_locked(bc, lba);
    if (e) {
        // 命中缓存，增加引用计数
        cache_ref_get(&e->ref);
        e->accessed = 1;
        bcache_bucket_unlock_irqrestore(bc, lba, bf);
        return e;
    }
    bcache_bucket_unlock_irqrestore(bc, lba, bf);

    uint64_t flags = spin_lock_irqsave(&bc->lock);
    e = bcache_find(bc, lba);
    if (e) {
        // 命中缓存，增加引用计数并移到 LRU 头部
        cache_ref_get(&e->ref);
        lru_remove(e);
        lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        return e;
    }

    spin_unlock_irqrestore(&bc->lock, flags);
    rw_mutex_write_lock(&bc->writeback_lock);
    flags = spin_lock_irqsave(&bc->lock);

    /* A concurrent miss may have populated this LBA while we waited. */
    e = bcache_find(bc, lba);
    if (e) {
        cache_ref_get(&e->ref);
        lru_remove(e);
        lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        rw_mutex_write_unlock(&bc->writeback_lock);
        return e;
    }

    // 缓存未命中，驱逐一个旧块
    e = bcache_evict(bc);
    if (!e) {
        spin_unlock_irqrestore(&bc->lock, flags);
        rw_mutex_write_unlock(&bc->writeback_lock);
        kdebug("[BCACHE] no evictable block lba=%lu\n", (unsigned long)lba);
        return NULL;
    }
    uint64_t old_lba = e->lba;
    int old_dirty = e->dirty && e->valid;
    e->valid = 0;
    spin_unlock_irqrestore(&bc->lock, flags);

    if (old_dirty && bc->dev) {
        int write_ret = bc->dev->write_sector(bc->dev, old_lba, e->data, 1);
        if (write_ret < 0) {
            kdebug("[BCACHE] writeback error lba=%lu\n", (unsigned long)old_lba);
            bcache_note_write_error(bc);
            flags = spin_lock_irqsave(&bc->lock);
            e->ref = 0;
            bcache_set_block_dirty_locked(bc, e, 1);
            e->valid = 1;
            bcache_hash_insert(bc, e);
            lru_insert_front(bc, e);
            spin_unlock_irqrestore(&bc->lock, flags);
            rw_mutex_write_unlock(&bc->writeback_lock);
            return NULL;
        }
    }
    rw_mutex_write_unlock(&bc->writeback_lock);

    // 从磁盘读取数据
    if (bc->dev) {
        int r = bc->dev->read_sector(bc->dev, lba, e->data, 1);
        if (r < 0) {
            kdebug("[BCACHE] read error lba=%lu\n", (unsigned long)lba);
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
        cache_ref_get(&dup->ref);
        lru_remove(dup);
        lru_insert_front(bc, dup);
        spin_unlock_irqrestore(&bc->lock, flags);
        return dup;
    }
    e->lba = lba;
    bcache_set_block_dirty_locked(bc, e, 0);
    e->dirty_gen = 0;
    e->valid = 1;

    bcache_hash_insert(bc, e);
    lru_insert_front(bc, e);
    spin_unlock_irqrestore(&bc->lock, flags);
    return e;
}

// 释放块引用（减少引用计数）
void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    cache_ref_put(&e->ref);
}

// 标记块为脏（数据已修改，需要写回磁盘）
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
/* Sorted-array membership for bcache_sync_scoped().  NULL page_nos means
 * "include every page" (full sync). */
static int bcache_sync_page_selected(const uint64_t *page_nos, size_t count,
                                     uint64_t page_no)
{
    if (!page_nos)
        return 1;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (page_nos[mid] == page_no)
            return 1;
        if (page_nos[mid] < page_no)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/*
 * Flush every dirty page selected by @page_nos (sorted ascending, or NULL for
 * all) and report whether the storage device accepted all writes.  Journal
 * recovery must not claim success after a timed-out VirtIO request merely
 * because older callers used a void sync interface.
 */
static int bcache_sync_common(bcache_t *bc, const uint64_t *page_nos,
                              size_t page_count) {
    if (!bc || !bc->dev)
        return -EINVAL;
    /* A recently failed write means the device is still wedged; fail fast
     * instead of paying another driver-level timeout per dirty entry. */
    if (bcache_write_quarantined(bc))
        return -EIO;
    char tmp[BCACHE_BLOCK_SIZE];
    char *page_tmp = NULL;
    int page_indices[BCACHE_SYNC_BATCH_PAGES];
    uint64_t sync_nos[BCACHE_SYNC_BATCH_PAGES];
    uint64_t page_gens[BCACHE_SYNC_BATCH_PAGES];
    int first_error = 0;

    /* Preserve write ordering for mutable metadata pages.  Without this,
     * concurrent fsync and eviction can write an older bitmap snapshot after
     * a newer one and make allocated blocks appear free again. */
    rw_mutex_write_lock(&bc->writeback_lock);

    for (int i = 0; i < bc->page_pool_size; i++) {
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        if (bc->dirty_pages == 0) {
            spin_unlock_irqrestore(&bc->lock, flags);
            break;
        }
        if (!bc->page_pool[i].valid || !bc->page_pool[i].dirty ||
            !bcache_sync_page_selected(page_nos, page_count,
                                       bc->page_pool[i].page_no)) {
            spin_unlock_irqrestore(&bc->lock, flags);
            continue;
        }
        if (!page_tmp)
            page_tmp = (char *)kmalloc(PCACHE_PAGE_SIZE *
                                       BCACHE_SYNC_BATCH_PAGES);
        if (!page_tmp) {
            spin_unlock_irqrestore(&bc->lock, flags);
            first_error = -ENOMEM;
            break;
        }
        page_indices[0] = i;
        sync_nos[0] = bc->page_pool[i].page_no;
        page_gens[0] = bc->page_pool[i].dirty_gen;
        memcpy(page_tmp, bc->page_pool[i].data, PCACHE_PAGE_SIZE);
        spin_unlock_irqrestore(&bc->lock, flags);

        /* Fresh ext4 blocks and inode-table pages are normally allocated in
         * ascending physical order, and fresh cache entries follow that same
         * order in the page pool.  Merge only adjacent pool entries whose
         * physical page numbers are exactly contiguous and are both selected.
         * Each page keeps its own dirty generation so a concurrent writer
         * cannot be cleared by an older batched snapshot. */
        int batch_pages = 1;
        while (batch_pages < BCACHE_SYNC_BATCH_PAGES &&
               i + batch_pages < bc->page_pool_size) {
            int index = i + batch_pages;
            flags = spin_lock_irqsave(&bc->lock);
            pcache_entry_t *candidate = &bc->page_pool[index];
            if (!candidate->valid || !candidate->dirty ||
                !bcache_sync_page_selected(page_nos, page_count,
                                           candidate->page_no) ||
                candidate->page_no != sync_nos[batch_pages - 1] + 1) {
                spin_unlock_irqrestore(&bc->lock, flags);
                break;
            }
            page_indices[batch_pages] = index;
            sync_nos[batch_pages] = candidate->page_no;
            page_gens[batch_pages] = candidate->dirty_gen;
            memcpy(page_tmp + (size_t)batch_pages * PCACHE_PAGE_SIZE,
                   candidate->data, PCACHE_PAGE_SIZE);
            spin_unlock_irqrestore(&bc->lock, flags);
            batch_pages++;
        }

        uint64_t lba =
            (sync_nos[0] * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
        int write_ret = bc->dev->write_sector(
            bc->dev, lba, page_tmp,
            (size_t)batch_pages * PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE);
        if (write_ret >= 0) {
            flags = spin_lock_irqsave(&bc->lock);
            for (int page = 0; page < batch_pages; page++) {
                pcache_entry_t *entry =
                    &bc->page_pool[page_indices[page]];
                if (entry->valid && entry->page_no == sync_nos[page] &&
                    entry->dirty_gen == page_gens[page])
                    bcache_set_page_dirty_locked(bc, entry, 0);
            }
            spin_unlock_irqrestore(&bc->lock, flags);
        } else {
            /* Abort early: the driver already exhausted its retry budget on
             * this request, so flushing the remaining entries would only
             * multiply the stall.  They stay dirty for the next sync. */
            first_error = write_ret;
            bcache_note_write_error(bc);
            a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
            a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES,
                         (uint64_t)batch_pages);
            break;
        }
        a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
        a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES,
                     (uint64_t)batch_pages);
        i += batch_pages - 1;
    }
    if (page_tmp)
        kfree(page_tmp);

    for (int i = 0; !first_error && i < bc->pool_size; i++) {
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        if (bc->dirty_blocks == 0) {
            spin_unlock_irqrestore(&bc->lock, flags);
            break;
        }
        if (!bc->pool[i].valid || !bc->pool[i].dirty) {
            spin_unlock_irqrestore(&bc->lock, flags);
            continue;
        }
        uint64_t lba = bc->pool[i].lba;
        uint64_t dirty_gen = bc->pool[i].dirty_gen;
        memcpy(tmp, bc->pool[i].data, BCACHE_BLOCK_SIZE);
        spin_unlock_irqrestore(&bc->lock, flags);

        int write_ret = bc->dev->write_sector(bc->dev, lba, tmp, 1);
        if (write_ret >= 0) {
            flags = spin_lock_irqsave(&bc->lock);
            if (bc->pool[i].valid && bc->pool[i].lba == lba &&
                bc->pool[i].dirty_gen == dirty_gen)
                bcache_set_block_dirty_locked(bc, &bc->pool[i], 0);
            spin_unlock_irqrestore(&bc->lock, flags);
        } else {
            first_error = write_ret;
            bcache_note_write_error(bc);
            break;
        }
    }
    rw_mutex_write_unlock(&bc->writeback_lock);
    return first_error;
}

int bcache_sync_checked(bcache_t *bc) {
    return bcache_sync_common(bc, NULL, 0);
}

/*
 * fsync() scope: flush only the dirty 4 KiB pages whose page_no appears in
 * @page_nos (sorted ascending, deduplicated), plus any dirty 512-byte
 * metadata blocks.  Everything else dirty is left for its own fsync or a full
 * sync, so one file's fsync no longer writes the whole mount's block cache.
 * Ordering (writeback_lock + per-page dirty_gen) is identical to a full sync.
 */
int bcache_sync_scoped(bcache_t *bc, const uint64_t *page_nos, size_t count) {
    return bcache_sync_common(bc, page_nos, count);
}

// 兼容不需要向上传播错误的历史 fsync/unmount 调用点。
void bcache_sync(bcache_t *bc) {
    (void)bcache_sync_checked(bc);
}

// 使缓存中的块失效（磁盘上的数据已改变）
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

static int pcache_flush_run(bcache_t *bc, pcache_entry_t *first)
{
    if (!bc->dev || !first || !first->dirty)
        return 0;

    pcache_entry_t *entries[BCACHE_SYNC_BATCH_PAGES];
    uint64_t page_nos[BCACHE_SYNC_BATCH_PAGES];
    uint64_t page_gens[BCACHE_SYNC_BATCH_PAGES];
    uint64_t flags = spin_lock_irqsave(&bc->lock);
    if (!first->valid || !first->dirty) {
        spin_unlock_irqrestore(&bc->lock, flags);
        return 0;
    }

    entries[0] = first;
    page_nos[0] = first->page_no;
    page_gens[0] = first->dirty_gen;
    memcpy(bc->writeback_buffer, first->data, PCACHE_PAGE_SIZE);
    int pages = 1;
    while (pages < BCACHE_SYNC_BATCH_PAGES) {
        pcache_entry_t *next = pcache_find(bc, page_nos[pages - 1] + 1);
        if (!next || !next->valid || !next->dirty)
            break;
        entries[pages] = next;
        page_nos[pages] = next->page_no;
        page_gens[pages] = next->dirty_gen;
        memcpy(bc->writeback_buffer + (size_t)pages * PCACHE_PAGE_SIZE,
               next->data, PCACHE_PAGE_SIZE);
        pages++;
    }
    spin_unlock_irqrestore(&bc->lock, flags);

    uint64_t lba = (page_nos[0] * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
    int r = bc->dev->write_sector(
        bc->dev, lba, bc->writeback_buffer,
        (size_t)pages * PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE);
    if (r >= 0) {
        flags = spin_lock_irqsave(&bc->lock);
        for (int i = 0; i < pages; i++) {
            pcache_entry_t *entry = entries[i];
            if (entry->valid && entry->page_no == page_nos[i] &&
                entry->dirty_gen == page_gens[i])
                bcache_set_page_dirty_locked(bc, entry, 0);
        }
        spin_unlock_irqrestore(&bc->lock, flags);
    }
    a20_perf_count(A20_PERF_PCACHE_WRITEBACK_IOS);
    a20_perf_add(A20_PERF_PCACHE_WRITEBACK_PAGES, (uint64_t)pages);
    return r;
}

static pcache_entry_t *pcache_evict_locked(bcache_t *bc) {
    int quarantined = bcache_write_quarantined(bc);
    pcache_entry_t *e = bc->page_lru_tail.prev;
    while (e != &bc->page_lru_head) {
        if (cache_ref_read(&e->ref) == 0) {
            /* Prefer clean victims while the device is known-wedged; a dirty
             * victim would force a flush that can only time out again. */
            if (quarantined && e->valid && e->dirty) {
                e = e->prev;
                continue;
            }
            if (e->valid) {
                uint64_t bf = pcache_bucket_lock_irqsave(bc, e->page_no);
                if (cache_ref_read(&e->ref) == 0 && e->valid &&
                    !(quarantined && e->dirty)) {
                    if (e->accessed) {
                        e->accessed = 0;
                        pcache_bucket_unlock_irqrestore(bc, e->page_no, bf);
                        e = e->prev;
                        continue;
                    }
                    page_lru_remove(e);
                    pcache_hash_remove_locked(bc, e);
                    e->ref = 1;
                    pcache_bucket_unlock_irqrestore(bc, e->page_no, bf);
                    return e;
                }
                pcache_bucket_unlock_irqrestore(bc, e->page_no, bf);
                e = e->prev;
                continue;
            }
            page_lru_remove(e);
            e->ref = 1;
            return e;
        }
        e = e->prev;
    }
    return NULL;
}

static pcache_entry_t *pcache_get(bcache_t *bc, uint64_t page_no,
                                  const void *full_overwrite) {
    /* Warm hit fast path: bucket lock only. */
    uint64_t bf = pcache_bucket_lock_irqsave(bc, page_no);
    pcache_entry_t *e = pcache_find_locked(bc, page_no);
    if (e) {
        cache_ref_get(&e->ref);
        e->accessed = 1;
        pcache_bucket_unlock_irqrestore(bc, page_no, bf);
        return e;
    }
    pcache_bucket_unlock_irqrestore(bc, page_no, bf);

    uint64_t flags = spin_lock_irqsave(&bc->lock);
    e = pcache_find(bc, page_no);
    if (e) {
        cache_ref_get(&e->ref);
        page_lru_remove(e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        return e;
    }

    spin_unlock_irqrestore(&bc->lock, flags);

    /* A single filesystem-wide fill lock made every cold page miss wait for
     * the preceding synchronous disk read.  Hash by page number so only
     * same-page (or same-shard) fills serialize; writeback_lock still protects
     * eviction and write ordering. */
    a20_perf_count(A20_PERF_PCACHE_FILL_MISSES);
    mutex_t *fill_lock =
        &bc->fill_locks[pcache_hash_key(page_no) & (PCACHE_FILL_LOCKS - 1)];
    if (!mutex_trylock(fill_lock)) {
        a20_perf_count(A20_PERF_PCACHE_FILL_CONTENDED);
        mutex_lock(fill_lock);
    }
    flags = spin_lock_irqsave(&bc->lock);

    /* Another miss may have completed while this one waited for its shard. */
    e = pcache_find(bc, page_no);
    if (e) {
        cache_ref_get(&e->ref);
        page_lru_remove(e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        mutex_unlock(fill_lock);
        return e;
    }

    spin_unlock_irqrestore(&bc->lock, flags);
    rw_mutex_write_lock(&bc->writeback_lock);
    flags = spin_lock_irqsave(&bc->lock);

    e = pcache_find(bc, page_no);
    if (e) {
        cache_ref_get(&e->ref);
        page_lru_remove(e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        rw_mutex_write_unlock(&bc->writeback_lock);
        mutex_unlock(fill_lock);
        return e;
    }

    e = pcache_evict_locked(bc);
    if (!e) {
        size_t valid = 0;
        size_t dirty = 0;
        size_t referenced = 0;
        uint64_t total_refs = 0;
        int max_refs = 0;
        for (int i = 0; i < bc->page_pool_size; i++) {
            pcache_entry_t *page = &bc->page_pool[i];
            int refs = cache_ref_read(&page->ref);
            if (page->valid)
                valid++;
            if (page->valid && page->dirty)
                dirty++;
            if (refs > 0) {
                referenced++;
                total_refs += (uint64_t)refs;
                if (refs > max_refs)
                    max_refs = refs;
            }
        }
        spin_unlock_irqrestore(&bc->lock, flags);
        printf("[BCACHE] no evictable page page=%lu valid=%lu dirty=%lu "
               "referenced=%lu total_refs=%lu max_refs=%d\n",
               (unsigned long)page_no, (unsigned long)valid,
               (unsigned long)dirty, (unsigned long)referenced,
               (unsigned long)total_refs, max_refs);
        rw_mutex_write_unlock(&bc->writeback_lock);
        mutex_unlock(fill_lock);
        return NULL;
    }

    uint64_t old_page = e->page_no;
    int old_dirty = e->dirty && e->valid;
    spin_unlock_irqrestore(&bc->lock, flags);

    int flush_ret = 0;
    if (old_dirty)
        flush_ret = pcache_flush_run(bc, e);
    if (old_dirty && flush_ret < 0) {
        bcache_note_write_error(bc);
        flags = spin_lock_irqsave(&bc->lock);
        e->page_no = old_page;
        e->valid = 1;
        bcache_set_page_dirty_locked(bc, e, 1);
        e->ref = 0;
        pcache_hash_insert(bc, e);
        page_lru_insert_front(bc, e);
        spin_unlock_irqrestore(&bc->lock, flags);
        rw_mutex_write_unlock(&bc->writeback_lock);
        mutex_unlock(fill_lock);
        return NULL;
    }
    flags = spin_lock_irqsave(&bc->lock);
    e->valid = 0;
    bcache_set_page_dirty_locked(bc, e, 0);
    spin_unlock_irqrestore(&bc->lock, flags);
    rw_mutex_write_unlock(&bc->writeback_lock);

    if (full_overwrite) {
        /* Initialize the miss before publishing it in the hash table.  This
         * avoids a redundant device read without allowing a concurrent reader
         * to observe the evicted page's old contents.  The caller retains the
         * entry reference until it marks the page dirty. */
        memcpy(e->data, full_overwrite, PCACHE_PAGE_SIZE);
        a20_perf_count(A20_PERF_PCACHE_FULL_OVERWRITE_SKIPS);
    } else if (bc->dev) {
        uint64_t lba = (page_no * PCACHE_PAGE_SIZE) / BCACHE_BLOCK_SIZE;
        if (bc->dev->read_sector(bc->dev, lba, e->data,
                                 PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE) < 0) {
            flags = spin_lock_irqsave(&bc->lock);
            e->ref = 0;
            e->page_no = (uint64_t)-1;
            page_lru_insert_front(bc, e);
            spin_unlock_irqrestore(&bc->lock, flags);
            mutex_unlock(fill_lock);
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
        cache_ref_get(&dup->ref);
        page_lru_remove(dup);
        page_lru_insert_front(bc, dup);
        spin_unlock_irqrestore(&bc->lock, flags);
        mutex_unlock(fill_lock);
        return dup;
    }

    e->page_no = page_no;
    bcache_set_page_dirty_locked(bc, e, 0);
    e->dirty_gen = 0;
    e->valid = 1;
    pcache_hash_insert(bc, e);
    page_lru_insert_front(bc, e);
    spin_unlock_irqrestore(&bc->lock, flags);
    mutex_unlock(fill_lock);
    return e;
}

static void pcache_release(pcache_entry_t *e) {
    if (!e) return;
    cache_ref_put(&e->ref);
}

/*
 * Read 2..16 aligned file-data pages with one device request.
 *
 * Clean file data already lives in the VFS page cache, so inserting the same
 * bytes into the block cache creates a second 4 KiB copy and another cache
 * lookup for every fault-around page.  Read cold data straight into the
 * caller's linear window instead.  Existing block-cache pages are overlaid
 * afterwards because they may contain a newer dirty version than the disk.
 *
 * The writeback mutex prevents a dirty entry from disappearing between the
 * device read and the coherent overlay.  Writers that update an existing page
 * publish under bc->lock, so the overlay observes either the version before or
 * after a genuinely concurrent write without ever exposing stale disk data in
 * place of a cache version that was already published.
 */
int bcache_read_bytes_batch(bcache_t *bc, uint64_t byte_off, void *buf,
                            size_t len) {
    if (!bc || !buf || len == 0)
        return len == 0 ? 0 : -EINVAL;
    if ((byte_off % PCACHE_PAGE_SIZE) != 0 ||
        (len % PCACHE_PAGE_SIZE) != 0) {
        return bcache_read_bytes(bc, byte_off, buf, len);
    }

    size_t page_count = len / PCACHE_PAGE_SIZE;
    if (page_count < 2 || page_count > BCACHE_READ_BATCH_MAX_PAGES)
        return bcache_read_bytes(bc, byte_off, buf, len);

    uint64_t first_page = byte_off / PCACHE_PAGE_SIZE;
    a20_perf_add(A20_PERF_PCACHE_FILL_MISSES, page_count);
    rw_mutex_read_lock(&bc->writeback_lock);
    int result = 0;
    if (bc->dev) {
        uint64_t lba = byte_off / BCACHE_BLOCK_SIZE;
        size_t sectors = len / BCACHE_BLOCK_SIZE;
        result = bc->dev->read_sector(bc->dev, lba, buf, sectors);
    } else {
        memset(buf, 0, len);
    }

    if (result >= 0) {
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        for (size_t i = 0; i < page_count; i++) {
            pcache_entry_t *entry = pcache_find(bc, first_page + i);
            if (entry) {
                memcpy((char *)buf + i * PCACHE_PAGE_SIZE, entry->data,
                       PCACHE_PAGE_SIZE);
                page_lru_remove(entry);
                page_lru_insert_front(bc, entry);
            }
        }
        spin_unlock_irqrestore(&bc->lock, flags);
    }
    rw_mutex_read_unlock(&bc->writeback_lock);
    return result < 0 ? result : 0;
}

// 读取字节数据（可能跨多个块）
#define READAHEAD_PAGES 1

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len) {
    if (len == 0)
        return 0;
    char *dst = (char *)buf;
    uint64_t first_page = byte_off / PCACHE_PAGE_SIZE;
    uint64_t last_page  = (byte_off + len - 1) / PCACHE_PAGE_SIZE;
    int sequential = (last_page - first_page + 1) >= 2;

    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t   off    = byte_off % PCACHE_PAGE_SIZE;
        size_t   chunk  = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;

        pcache_entry_t *e = pcache_get(bc, page_no, NULL);
        if (!e) return -1;
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        memcpy(dst, e->data + off, chunk);
        spin_unlock_irqrestore(&bc->lock, flags);
        pcache_release(e);

        dst      += chunk;
        byte_off += chunk;
        len      -= chunk;
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

// 写入字节数据（可能跨多个块，标记块为脏）
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len) {
    if (len == 0)
        return 0;
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t page_no = byte_off / PCACHE_PAGE_SIZE;
        size_t   off    = byte_off % PCACHE_PAGE_SIZE;
        size_t   chunk  = PCACHE_PAGE_SIZE - off;
        if (chunk > len) chunk = len;

        const void *full_overwrite =
            (off == 0 && chunk == PCACHE_PAGE_SIZE) ? src : NULL;
        pcache_entry_t *e = pcache_get(bc, page_no, full_overwrite);
        if (!e) return -1;
        uint64_t flags = spin_lock_irqsave(&bc->lock);
        memcpy(e->data + off, src, chunk);
        bcache_set_page_dirty_locked(bc, e, 1);
        spin_unlock_irqrestore(&bc->lock, flags);
        pcache_release(e);

        src      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}
