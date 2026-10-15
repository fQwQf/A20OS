#include <stdint.h>
#define CONFIG_EXT4_RUST_EXTENT_TXN 1
#include "../../kernel/fs/diskfs/ext4_sync.c"

static int read_result;
static unsigned reads;
static int scoped_result, full_result;
static unsigned scoped_calls, full_calls;
int bcache_read_bytes(bcache_t *bc, uint64_t offset, void *out, size_t len)
{
    (void)bc; (void)offset; (void)out; (void)len;
    reads++;
    return read_result;
}

int bcache_sync_scoped(bcache_t *bc, const uint64_t *pages, size_t count)
{
    if (!bc || !pages || !count)
        return -EINVAL;
    scoped_calls++;
    return scoped_result;
}

int bcache_sync_checked(bcache_t *bc)
{
    if (!bc)
        return -EINVAL;
    full_calls++;
    return full_result;
}

int main(void)
{
    ext4_sb_info_t sb = {0};
    uint8_t block[4096] = {0};
    sb.bc = (bcache_t *)(uintptr_t)1;
    sb.block_size = sizeof(block);
    sb.blocks_count = 16;
    if (ext4_sync_plan_read_block(&sb, 3, block, sizeof(block)) != 0)
        return 1;
    read_result = -1; /* legacy cache read errors are generic negative status */
    if (ext4_sync_plan_read_block(&sb, 3, block, sizeof(block)) != -EIO)
        return 2;
    read_result = 0;
    unsigned before = reads;
    if (ext4_sync_plan_read_block(&sb, 16, block, sizeof(block)) != -EIO)
        return 3;
    if (reads != before)
        return 4;
    /* This block is still below blocks_count, so the failure must be the
     * checked block*block_size multiplication, not the range guard. */
    sb.blocks_count = UINT64_MAX;
    uint64_t overflow_block = UINT64_MAX / sizeof(block) + 1;
    before = reads;
    if (ext4_sync_plan_read_block(&sb, overflow_block, block, sizeof(block)) != -EIO)
        return 5;
    if (reads != before)
        return 6;
    uint64_t pages[] = {2, 7};
    if (ext4_sync_plan_flush_scoped(&sb, pages, 2) != scoped_result ||
        scoped_calls != 1 || full_calls != 0)
        return 7;
    scoped_result = -EIO;
    if (ext4_sync_plan_flush_scoped(&sb, pages, 2) != -EIO ||
        scoped_calls != 2)
        return 8;
    full_result = -117;
    if (ext4_sync_plan_flush_full(&sb) != -117 || full_calls != 1)
        return 9;
    return 0;
}
