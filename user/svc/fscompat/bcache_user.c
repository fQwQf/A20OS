/*
 * fscompat/bcache_user.c — the user-space implementation of the block_cache
 * API.
 *
 * The contract matches kernel/fs/block_cache.c: byte-granular reads and
 * writes, caching in 512B sector lines, write-through (every write_bytes
 * goes straight to the block device and refreshes the cached copy, so sync
 * is a no-op). The host is single-threaded, so the kernel version's bucket
 * locks, LRU and writeback queue are unnecessary; a small pool suffices for
 * metadata plus sequential data access.
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

/* The real bcache_t is defined by the kernel header and is large; the host
 * only passes the handle around and correlates internal state through a
 * side table. */
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
        /* Direct-mapped conflict: evict.  Under write-through semantics no
         * dirty copy exists. */
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
                return -1; /* a partial-sector write needs the existing
                               * content, so a read failure is a failure */
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
    return 0; /* write-through: no dirty data pending flush */
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

int bcache_sync_held(bcache_t *bc)
{
    /* This cache is write-through: bcache_write_bytes() already put every
     * byte on the device, so there is no buffered metadata for a transaction
     * to hold back.  The journal's ordering guarantee is a property of the
     * kernel's page cache, and the kernel is what owns the disk image here. */
    (void)bc;
    return 0;
}

void bcache_hold_page(bcache_t *bc, uint64_t page_no)
{
    (void)bc;
    (void)page_no;
}

void bcache_release_holds(bcache_t *bc)
{
    (void)bc;
}

size_t bcache_held_pages(const bcache_t *bc)
{
    (void)bc;
    return 0;
}

void bcache_set_sync_hook(bcache_t *bc, int (*hook)(bcache_t *))
{
    (void)bc;
    (void)hook;
}

void bcache_invalidate_page(bcache_t *bc, uint64_t page_no)
{
    struct bcache_compat_state *st = state_of(bc);
    if (!st)
        return;
    ubc_line_t *ln = &st->lines[(uint32_t)(page_no % UBC_LINES)];
    if (ln->valid && ln->lba == page_no)
        ln->valid = 0;
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
