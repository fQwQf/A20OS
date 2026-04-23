#include "block_cache.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"

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

// 创建块缓存（分配 8192 个 512 字节的块）
bcache_t *bcache_create(block_dev_t *dev) {
    bcache_t *bc = (bcache_t *)kmalloc(sizeof(bcache_t));
    if (!bc) return NULL;
    memset(bc, 0, sizeof(*bc));

    bc->dev = dev;  // 绑定底层块设备
    bc->pool_size = BCACHE_MAX_BLOCKS;
    bc->pool = (bcache_entry_t *)kmalloc(sizeof(bcache_entry_t) * bc->pool_size);
    if (!bc->pool) { kfree(bc); return NULL; }

    // 初始化 LRU 链表（头尾哨兵节点）
    bc->lru_head.prev = NULL;
    bc->lru_head.next = &bc->lru_tail;
    bc->lru_tail.next = NULL;
    bc->lru_tail.prev = &bc->lru_head;

    // 初始化所有缓存条目
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

// 销毁块缓存（同步所有脏块并释放内存）
void bcache_destroy(bcache_t *bc) {
    if (!bc) return;
    bcache_sync(bc);  // 先同步所有脏块到磁盘
    if (bc->pool) kfree(bc->pool);
    kfree(bc);
}

// 在缓存中查找指定的 LBA 块（线性搜索）
static bcache_entry_t *bcache_find(bcache_t *bc, uint64_t lba) {
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].valid && bc->pool[i].lba == lba) {
            return &bc->pool[i];
        }
    }
    return NULL;
}

// 驱逐一个块（LRU 淘汰算法：从尾部找最久未使用的块）
static bcache_entry_t *bcache_evict(bcache_t *bc) {
    bcache_entry_t *e = bc->lru_tail.prev;  // 从最久未使用的开始
    while (e != &bc->lru_head) {
        if (e->ref == 0) {  // 只能驱逐引用计数为 0 的块
            // 如果是脏块，先写回磁盘
            if (e->dirty && bc->dev && e->valid) {
                if (bc->dev->write_sector(bc->dev, e->lba, e->data, 1) < 0) {
                    e = e->prev;  // 写回失败，跳过这个块
                    continue;
                }
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

// 获取一个块（从缓存或从磁盘读取）
bcache_entry_t *bcache_get(bcache_t *bc, uint64_t lba) {
    bcache_entry_t *e = bcache_find(bc, lba);
    if (e) {
        // 命中缓存，增加引用计数并移到 LRU 头部
        e->ref++;
        lru_remove(e);
        lru_insert_front(bc, e);
        return e;
    }

    // 缓存未命中，驱逐一个旧块
    e = bcache_evict(bc);
    e->lba   = lba;
    e->dirty = 0;
    e->ref   = 1;
    e->valid = 0;

    // 从磁盘读取数据
    if (bc->dev) {
        int r = bc->dev->read_sector(bc->dev, lba, e->data, 1);
        if (r < 0) {
            printf("[BCACHE] read error lba=%lu\n", (unsigned long)lba);
            e->ref = 0;
            e->lba = (uint64_t)-1;
            lru_insert_front(bc, e);
            return NULL;
        }
    } else {
        // 没有底层设备，清零（用于内存文件系统）
        memset(e->data, 0, BCACHE_BLOCK_SIZE);
    }
    e->valid = 1;

    lru_insert_front(bc, e);
    return e;
}

// 释放块引用（减少引用计数）
void bcache_release(bcache_entry_t *e) {
    if (!e) return;
    if (e->ref > 0) e->ref--;
}

// 标记块为脏（数据已修改，需要写回磁盘）
void bcache_mark_dirty(bcache_entry_t *e) {
    if (e) e->dirty = 1;
}

// 同步所有脏块到磁盘（fsync 系统调用时使用）
void bcache_sync(bcache_t *bc) {
    if (!bc || !bc->dev) return;
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].valid && bc->pool[i].dirty) {
            if (bc->dev->write_sector(bc->dev, bc->pool[i].lba, bc->pool[i].data, 1) >= 0)
                bc->pool[i].dirty = 0;  // 写回成功，清除脏标志
        }
    }
}

// 使缓存中的块失效（磁盘上的数据已改变）
void bcache_invalidate(bcache_t *bc, uint64_t lba) {
    if (!bc) return;
    for (int i = 0; i < bc->pool_size; i++) {
        if (bc->pool[i].lba == lba) {
            bc->pool[i].valid = 0;
            bc->pool[i].dirty = 0;
        }
    }
}

// 读取字节数据（可能跨多个块）
int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len) {
    char *dst = (char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;  // 计算块号
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;  // 块内偏移
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;       // 本次读取的大小
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

// 写入字节数据（可能跨多个块，标记块为脏）
int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf, size_t len) {
    const char *src = (const char *)buf;
    while (len > 0) {
        uint64_t lba    = byte_off / BCACHE_BLOCK_SIZE;  // 计算块号
        size_t   off    = byte_off % BCACHE_BLOCK_SIZE;  // 块内偏移
        size_t   chunk  = BCACHE_BLOCK_SIZE - off;       // 本次写入的大小
        if (chunk > len) chunk = len;

        bcache_entry_t *e = bcache_get(bc, lba);
        if (!e) return -1;
        memcpy(e->data + off, src, chunk);
        bcache_mark_dirty(e);  // 标记为脏，需要写回磁盘
        bcache_release(e);

        src      += chunk;
        byte_off += chunk;
        len      -= chunk;
    }
    return 0;
}
