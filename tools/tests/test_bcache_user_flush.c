#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "fs/block_cache.h"

static int flush_result;
static unsigned flush_calls;

static int fake_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count)
{
    (void)dev;
    (void)lba;
    (void)buf;
    (void)count;
    return 0;
}

static int fake_write(block_dev_t *dev, uint64_t lba, const void *buf,
                      size_t count)
{
    (void)dev;
    (void)lba;
    (void)buf;
    (void)count;
    return 0;
}

static int fake_flush(block_dev_t *dev)
{
    (void)dev;
    flush_calls++;
    return flush_result;
}

int main(void)
{
    block_dev_t dev = {
        .read_sector = fake_read,
        .write_sector = fake_write,
        .flush = fake_flush,
        .capacity = 65536,
        .sector_size = 512,
    };
    bcache_t *bc = bcache_create(&dev);
    assert(bc != NULL);
    assert(bc->dev == &dev);

    flush_result = 0;
    assert(bcache_sync_checked(bc) == 0);
    assert(flush_calls == 1);

    flush_result = -95; /* callback exists but device rejects flush */
    assert(bcache_sync_checked(bc) == -95);
    assert(flush_calls == 2);

    flush_result = -5;
    assert(bcache_sync_scoped(bc, NULL, 0) == -5);
    assert(flush_calls == 3);

    /* Optional callback compatibility applies only when the callback is
     * absent. An implemented callback's errno is never swallowed. */
    dev.flush = NULL;
    assert(bcache_sync_checked(bc) == 0);
    assert(bcache_sync_scoped(bc, NULL, 0) == 0);
    assert(flush_calls == 3);

    bcache_destroy(bc);
    assert(bc->dev == NULL);
    assert(bcache_sync_checked(bc) < 0);
    return 0;
}
