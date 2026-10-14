/* Narrow C ABI for the Rust-owned filesystem-host page cache. */
#include <stdint.h>
#include "core/types.h"
#include "core/stdio.h"
#include "core/sync.h"
#include "core/string.h"
#include "fs/block_cache.h"

extern void a20_ufsd_test_log(const char *line);

#define UBC_MAX_INSTANCES 8
static bcache_t g_handles[UBC_MAX_INSTANCES];

/* A test-only one-shot failure on the Rust checkpoint sync path. The disk
 * write has completed, but its flush reports EIO; the page stays dirty and
 * the next checkpoint retry must issue the write and flush again. */
int32_t a20_ubc_test_fail_checkpoint_flush(void)
{
#ifdef CONFIG_UFSD_JBD2_FAIL_CHECKPOINT_ONCE
    static int injected;
    if (!injected) {
        injected = 1;
        a20_ufsd_test_log("[UFSD TEST] checkpoint flush EIO hook hit");
        return 1;
    }
#endif
    return 0;
}

extern int32_t a20_ubc_create(uint32_t index, void *dev);
extern void a20_ubc_destroy(uint32_t index);
extern int32_t a20_ubc_read(uint32_t index, uint64_t byte_off,
                            uint8_t *out, size_t len);
extern int32_t a20_ubc_write(uint32_t index, uint64_t byte_off,
                             const uint8_t *input, size_t len);
extern int32_t a20_ubc_sync(uint32_t index, uint32_t mode,
                            const uint64_t *pages, size_t count);
extern void a20_ubc_hold(uint32_t index, uint64_t page_no);
extern void a20_ubc_release_holds(uint32_t index);
extern size_t a20_ubc_held(uint32_t index);
extern void a20_ubc_invalidate(uint32_t index, uint64_t page_no);

static int index_of(const bcache_t *bc)
{
    for (int i = 0; i < UBC_MAX_INSTANCES; i++)
        if (&g_handles[i] == bc && g_handles[i].dev)
            return i;
    return -1;
}

int32_t a20_ubc_read_sector(void *dev, uint64_t lba, uint8_t *out,
                            uint32_t count)
{
    block_dev_t *block = (block_dev_t *)dev;
    return block && block->read_sector &&
           block->read_sector(block, lba, out, count) == 0 ? 0 : -1;
}

int32_t a20_ubc_write_sector(void *dev, uint64_t lba, const uint8_t *input,
                             uint32_t count)
{
    block_dev_t *block = (block_dev_t *)dev;
    return block && block->write_sector &&
           block->write_sector(block, lba, input, count) == 0 ? 0 : -1;
}

int32_t a20_ubc_flush_device(void *dev, int32_t *has_flush)
{
    block_dev_t *block = (block_dev_t *)dev;
    if (!has_flush)
        return -22;
    *has_flush = block && block->flush ? 1 : 0;
    if (!*has_flush)
        return -95;
    int rc = block->flush(block);
    return rc == 0 ? 0 : (rc < 0 ? rc : -5);
}

bcache_t *bcache_create(block_dev_t *dev)
{
    if (!dev)
        return NULL;
    for (uint32_t i = 0; i < UBC_MAX_INSTANCES; i++) {
        bcache_t *bc = &g_handles[i];
        if (bc->dev)
            continue;
        memset(bc, 0, sizeof(*bc));
        bc->dev = dev;
        spin_init(&bc->lock);
        mutex_init(&bc->fill_locks[0]);
        rw_mutex_init(&bc->writeback_lock);
        if (a20_ubc_create(i, dev) < 0) {
            memset(bc, 0, sizeof(*bc));
            return NULL;
        }
        return bc;
    }
    return NULL;
}

void bcache_destroy(bcache_t *bc)
{
    int i = index_of(bc);
    if (i < 0)
        return;
    /* A failed final writeback leaves the Rust cache allocated and the
     * handle live, preserving data for an explicit retry instead of freeing
     * the only dirty copy through this void legacy API. */
    if (bcache_sync_checked(bc) < 0)
        return;
    a20_ubc_destroy((uint32_t)i);
    memset(bc, 0, sizeof(*bc));
}

int bcache_read_bytes(bcache_t *bc, uint64_t byte_off, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    int i = index_of(bc);
    if (i < 0 || !buf)
        return -1;
    return a20_ubc_read((uint32_t)i, byte_off, buf, len);
}

int bcache_read_bytes_batch(bcache_t *bc, uint64_t byte_off, void *buf,
                            size_t len)
{
    return bcache_read_bytes(bc, byte_off, buf, len);
}

int bcache_write_bytes(bcache_t *bc, uint64_t byte_off, const void *buf,
                       size_t len)
{
    if (len == 0)
        return 0;
    int i = index_of(bc);
    if (i < 0 || !buf)
        return -1;
    return a20_ubc_write((uint32_t)i, byte_off, buf, len);
}

static int sync_mode(bcache_t *bc, uint32_t mode, const uint64_t *pages,
                     size_t count)
{
    int i = index_of(bc);
    if (i < 0)
        return -1;
    if (bc->sync_hook && bc->sync_hook_depth == 0) {
        bc->sync_hook_depth++;
        int rc = bc->sync_hook(bc);
        bc->sync_hook_depth--;
        if (rc < 0)
            return rc;
    }
    return a20_ubc_sync((uint32_t)i, mode, pages, count);
}

int bcache_sync_checked(bcache_t *bc) { return sync_mode(bc, 0, NULL, 0); }
void bcache_sync(bcache_t *bc) { (void)bcache_sync_checked(bc); }
int bcache_sync_scoped(bcache_t *bc, const uint64_t *pages, size_t count)
{ return sync_mode(bc, 2, pages, count); }
int bcache_sync_held(bcache_t *bc) { return sync_mode(bc, 1, NULL, 0); }

void bcache_hold_page(bcache_t *bc, uint64_t page_no)
{
    int i = index_of(bc);
    if (i >= 0) a20_ubc_hold((uint32_t)i, page_no);
}
void bcache_release_holds(bcache_t *bc)
{
    int i = index_of(bc);
    if (i >= 0) a20_ubc_release_holds((uint32_t)i);
}
size_t bcache_held_pages(const bcache_t *bc)
{
    int i = index_of(bc);
    return i >= 0 ? a20_ubc_held((uint32_t)i) : 0;
}
void bcache_set_sync_hook(bcache_t *bc, int (*hook)(bcache_t *), void *owner)
{
    if (!bc) return;
    bc->sync_hook = hook;
    bc->owner = hook ? owner : NULL;
}
void bcache_invalidate_page(bcache_t *bc, uint64_t page_no)
{
    int i = index_of(bc);
    if (i >= 0) a20_ubc_invalidate((uint32_t)i, page_no);
}
void bcache_invalidate(bcache_t *bc, uint64_t lba)
{
    int i = index_of(bc);
    if (i >= 0) a20_ubc_invalidate((uint32_t)i, lba / 8);
}
void bcache_get_stats(bcache_stats_t *stats)
{
    if (stats) memset(stats, 0, sizeof(*stats));
}
