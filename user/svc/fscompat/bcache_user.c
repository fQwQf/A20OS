/*
 * fscompat/bcache_user.c — block_cache API 的用户态实现。
 *
 * 契约与 kernel/fs/block_cache.c 一致：字节粒度读写、按 512B 扇区行缓存、
 * 写穿透（每次 write_bytes 直达块设备并刷新缓存副本，sync 即无操作）。
 * 宿主单线程，无需内核版的桶锁/LRU/写回队列；容量取小池即可满足
 * 元数据 + 顺序数据访问。
 */
#include <stdint.h>
#include "core/types.h"
#include "core/sync.h"
#include "core/string.h"
#include "fs/block_cache.h"

#define UBC_LINES 128

typedef struct ubc_line {
    uint64_t lba;
    int      valid;
    uint32_t stamp;
    uint8_t  data[512];
} ubc_line_t;

struct bcache_compat_state {
    block_dev_t *dev;
    ubc_line_t   lines[UBC_LINES];
    uint32_t     clock;
};

/* 真实 bcache_t 由内核头定义且体积庞大；宿主只传递句柄，
 * 内部状态经 side-table 关联。 */
#define UBC_MAX_INSTANCES 8
static struct bcache_compat_state g_states[UBC_MAX_INSTANCES];
static bcache_t g_handles[UBC_MAX_INSTANCES];

static struct bcache_compat_state *state_of(bcache_t *bc)
{
    for (int i = 0; i < UBC_MAX_INSTANCES; i++)
        if (&g_handles[i] == bc)
            return &g_states[i];
    return NULL;
}

bcache_t *bcache_create(block_dev_t *dev)
{
    for (int i = 0; i < UBC_MAX_INSTANCES; i++) {
        if (!g_states[i].dev) {
            memset(&g_states[i], 0, sizeof(g_states[i]));
            g_states[i].dev = dev;
            spin_init(&g_handles[i].lock);
            mutex_init(&g_handles[i].fill_locks[0]);
            rw_mutex_init(&g_handles[i].writeback_lock);
            return &g_handles[i];
        }
    }
    return NULL;
}

void bcache_destroy(bcache_t *bc)
{
    struct bcache_compat_state *st = state_of(bc);
    if (st)
        st->dev = NULL;
}

static ubc_line_t *line_for(struct bcache_compat_state *st, uint64_t lba)
{
    uint32_t idx = (uint32_t)(lba % UBC_LINES);
    ubc_line_t *ln = &st->lines[idx];
    if (ln->valid && ln->lba != lba) {
        /* 直接映射冲突：逐出。写穿透语义下脏副本不存在。 */
        ln->valid = 0;
    }
    if (!ln->valid) {
        if (st->dev->read_sector(st->dev, lba, ln->data, 1) != 0)
            return NULL;
        ln->lba = lba;
        ln->valid = 1;
    }
    ln->stamp = ++st->clock;
    return ln;
}

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len)
{
    struct bcache_compat_state *st = state_of(bc);
    if (!st || !len)
        return -1;

    uint64_t lba = byte_off / 512;
    uint32_t off = (uint32_t)(byte_off % 512);
    uint8_t *out = buf;

    while (len) {
        ubc_line_t *ln = line_for(st, lba);
        if (!ln)
            return -1;
        uint32_t chunk = 512 - off;
        if (chunk > len)
            chunk = (uint32_t)len;
        memcpy(out, ln->data + off, chunk);
        out += chunk;
        len -= chunk;
        lba++;
        off = 0;
    }
    return 0;
}

int bcache_read_bytes_batch(bcache_t *bc, uint64_t byte_off, void *buf,
                            size_t len)
{
    return bcache_read_bytes(bc, byte_off, buf, len);
}

int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf,
                       size_t len)
{
    struct bcache_compat_state *st = state_of(bc);
    if (!st || !len)
        return -1;

    uint64_t lba = byte_off / 512;
    uint32_t off = (uint32_t)(byte_off % 512);
    const uint8_t *in = buf;
    uint8_t sect[512];

    while (len) {
        uint32_t chunk = 512 - off;
        if (chunk > len)
            chunk = (uint32_t)len;

        ubc_line_t *ln = line_for(st, lba);
        if (!ln) {
            if (!(off == 0 && chunk == 512))
                return -1; /* 部分扇区写需要既有内容，读失败即失败 */
            memset(sect, 0, sizeof(sect));
        } else {
            memcpy(sect, ln->data, 512);
        }
        memcpy(sect + off, in, chunk);

        if (st->dev->write_sector(st->dev, lba, sect, 1) != 0)
            return -1;
        if (ln)
            memcpy(ln->data, sect, 512);

        in += chunk;
        len -= chunk;
        lba++;
        off = 0;
    }
    return 0;
}

int bcache_sync_checked(bcache_t *bc)
{
    (void)bc;
    return 0; /* 写穿透：无待刷脏数据 */
}

void bcache_sync(bcache_t *bc)
{
    (void)bc;
}

int bcache_sync_scoped(bcache_t *bc, const uint64_t *page_nos,
                       size_t count)
{
    (void)bc;
    (void)page_nos;
    (void)count;
    return 0;
}

void bcache_invalidate(bcache_t *bc, uint64_t lba)
{
    struct bcache_compat_state *st = state_of(bc);
    if (!st)
        return;
    ubc_line_t *ln = &st->lines[(uint32_t)(lba % UBC_LINES)];
    if (ln->valid && ln->lba == lba)
        ln->valid = 0;
}

void bcache_get_stats(bcache_stats_t *stats)
{
    if (stats)
        memset(stats, 0, sizeof(*stats));
}
