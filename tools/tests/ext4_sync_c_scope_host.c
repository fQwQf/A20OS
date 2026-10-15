#include <stdint.h>
#include <stddef.h>
#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/block_cache.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/klog.h"

/* Include the C-reference planner itself, rather than a reimplementation. */
#include "../../kernel/fs/diskfs/ext4_sync.c"

static int checked_calls;
static int commit_calls;
static int checked_result;
static int commit_result;

int ext4_journal_sync_hook(bcache_t *bc)
{
    (void)bc;
    return 0;
}

int bcache_sync_checked(bcache_t *bc)
{
    (void)bc;
    checked_calls++;
    return checked_result;
}

int ext4_journal_commit(ext4_sb_info_t *sb)
{
    (void)sb;
    commit_calls++;
    return commit_result;
}

int main(void)
{
    bcache_t cache = {0};
    ext4_sb_info_t sync_sb = { .bc = &cache, .journal = (void *)1 };
    cache.sync_hook = ext4_journal_sync_hook;
    checked_result = -5;
    if (ext4_sync_commit_before_plan(&sync_sb) != -5 ||
        checked_calls != 1 || commit_calls != 0)
        return 3;
    cache.sync_hook = NULL;
    commit_result = -7;
    if (ext4_sync_commit_before_plan(&sync_sb) != -7 ||
        checked_calls != 1 || commit_calls != 1)
        return 4;
    cache.sync_hook = (int (*)(bcache_t *))1;
    if (ext4_sync_commit_before_plan(&sync_sb) != -7 ||
        checked_calls != 1 || commit_calls != 2)
        return 5;

    ext4_group_desc_t desc[2] = {0};
    desc[1].bg_block_bitmap_lo = 130;
    desc[1].bg_inode_bitmap_lo = 131;
    desc[1].bg_inode_table_lo = 132;
    ext4_sb_info_t sb = {0};
    sb.group_descs = desc;
    sb.groups_count = 2;
    sb.inodes_count = 256;
    sb.inodes_per_group = 128;
    sb.inode_size = 256;
    sb.block_size = 4096;
    sb.blocks_count = 1024;
    sb.desc_size = 64;
    sb.block_group_desc_table_byte = 2048;

    uint64_t pages[16] = {0};
    size_t count = 0;
    if (ext4_sync_group_meta(&sb, 1, 130, pages, &count, 16) != 0)
        return 1;
    int inode_page = 0, next_table_page = 0;
    for (size_t i = 0; i < count; i++) {
        inode_page |= pages[i] == 132;
        next_table_page |= pages[i] == 133;
    }
    if (!inode_page || next_table_page)
        return 2;
    return 0;
}
