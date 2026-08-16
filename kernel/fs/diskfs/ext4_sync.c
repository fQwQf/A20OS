#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/block_cache.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/klog.h"

/* Scoped fsync: make one file durable without flushing every dirty page of
 * the whole mount's block cache.  The VFS calls ext4_vn_sync() through
 * vnode_ops.sync_vnode after the 4 KiB page cache has written the file's data
 * into the block cache.  We collect the exact 4 KiB block-cache page numbers
 * this file needs -- its data extents, the inode-table page holding its inode,
 * and the allocation metadata (bitmaps/group descriptors) of the block groups
 * it occupies -- and flush only those pages with bcache_sync_scoped().
 *
 * Correctness: a crash after fsync() must not lose the file's data, its inode,
 * or the bitmap marking its blocks allocated (a stale bitmap would let a later
 * allocation reuse blocks whose data was already made durable).  All of those
 * are covered by the collected page set.  Other files' data and metadata are
 * left dirty, exactly as Linux fsync(file) leaves other inodes dirty.
 */

#define EXT4_SYNC_MAX_PAGES 2048

/* Append every 4 KiB page covering [byte_start, byte_start + byte_len).
 * Returns -1 when the bounded array is full (caller falls back to full sync). */
static int sync_add_byte_range(uint64_t *pages, size_t *n, size_t cap,
                               uint64_t byte_start, uint64_t byte_len)
{
    if (byte_len == 0)
        return 0;
    uint64_t first = byte_start / PCACHE_PAGE_SIZE;
    uint64_t last = (byte_start + byte_len - 1) / PCACHE_PAGE_SIZE;
    for (uint64_t p = first; p <= last; p++) {
        if (*n >= cap)
            return -1;
        pages[(*n)++] = p;
    }
    return 0;
}

/* Recursively collect the data pages of a depth-0/1 extent tree plus the
 * index blocks themselves.  Deeper trees are extremely rare for build files;
 * a depth > 1 signals the caller to fall back to a full mount sync. */
static int ext4_sync_extent_pages(ext4_sb_info_t *sb, const ext4_inode_t *inode,
                                  uint64_t *pages, size_t *n, size_t cap)
{
    const uint8_t *raw = (const uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0)
        return 0;
    if (hdr.eh_depth == 0) {
        int ne = hdr.eh_entries;
        if (ne > 4)
            ne = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), (size_t)ne * sizeof(ext4_extent_t));
        for (int i = 0; i < ne; i++) {
            uint16_t len = ext[i].ee_len;
            if (len > 0x8000)
                len -= 0x8000;
            if (!len)
                continue;
            uint64_t pb = (uint64_t)ext[i].ee_start_lo |
                          ((uint64_t)ext[i].ee_start_hi << 32);
            if (sync_add_byte_range(pages, n, cap, pb * sb->block_size,
                                    (uint64_t)len * sb->block_size) < 0)
                return -1;
        }
        return 0;
    }
    if (hdr.eh_depth > 1)
        return -1;

    int ni = hdr.eh_entries;
    if (ni > 4)
        ni = 4;
    ext4_extent_idx_t idx0[4];
    memcpy(idx0, raw + sizeof(hdr), (size_t)ni * sizeof(ext4_extent_idx_t));
    for (int i = 0; i < ni; i++) {
        uint64_t leaf = (uint64_t)idx0[i].ei_leaf_lo |
                        ((uint64_t)idx0[i].ei_leaf_hi << 32);
        if (sync_add_byte_range(pages, n, cap, leaf * sb->block_size,
                                sb->block_size) < 0)
            return -1;
        char *b = (char *)kmalloc(sb->block_size);
        if (!b)
            return -1;
        if (bcache_read_bytes(sb->bc, leaf * sb->block_size, b,
                              sb->block_size) < 0) {
            kfree(b);
            return -1;
        }
        ext4_extent_header_t eh;
        memcpy(&eh, b, sizeof(eh));
        if (eh.eh_magic == EXT4_EXT_MAGIC && eh.eh_depth == 0) {
            ext4_extent_t *ep = (ext4_extent_t *)(b + sizeof(eh));
            int cnt = eh.eh_entries;
            for (int j = 0; j < cnt; j++) {
                uint16_t len = ep[j].ee_len;
                if (len > 0x8000)
                    len -= 0x8000;
                if (!len)
                    continue;
                uint64_t pb = (uint64_t)ep[j].ee_start_lo |
                              ((uint64_t)ep[j].ee_start_hi << 32);
                if (sync_add_byte_range(pages, n, cap, pb * sb->block_size,
                                        (uint64_t)len * sb->block_size) < 0) {
                    kfree(b);
                    return -1;
                }
            }
        }
        kfree(b);
    }
    return 0;
}

/* Append the metadata pages of one block group: its block/inode bitmaps, its
 * inode-table span and the group descriptor block. */
static int ext4_sync_group_meta(ext4_sb_info_t *sb, uint32_t group,
                                uint64_t *pages, size_t *n, size_t cap)
{
    if (group >= sb->groups_count)
        return 0;
    const ext4_group_desc_t *gd = &sb->group_descs[group];
    uint64_t bbm = (uint64_t)gd->bg_block_bitmap_lo |
                   ((uint64_t)gd->bg_block_bitmap_hi << 32);
    uint64_t ibm = (uint64_t)gd->bg_inode_bitmap_lo |
                   ((uint64_t)gd->bg_inode_bitmap_hi << 32);
    uint64_t it = (uint64_t)gd->bg_inode_table_lo |
                  ((uint64_t)gd->bg_inode_table_hi << 32);
    uint64_t inode_bytes = (uint64_t)sb->inodes_per_group * sb->inode_size;
    uint64_t gdt_byte = sb->block_group_desc_table_byte +
                        (uint64_t)group * sb->desc_size;
    if (sync_add_byte_range(pages, n, cap, bbm * sb->block_size,
                            sb->block_size) < 0 ||
        sync_add_byte_range(pages, n, cap, ibm * sb->block_size,
                            sb->block_size) < 0 ||
        sync_add_byte_range(pages, n, cap, it * sb->block_size,
                            inode_bytes) < 0 ||
        sync_add_byte_range(pages, n, cap, gdt_byte, sb->desc_size) < 0)
        return -1;
    return 0;
}

int ext4_vn_sync(vnode_t *vn)
{
    if (!vn || !vn->fs_data)
        return -EINVAL;
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_sb_info_t *sb = p->sb;
    if (!sb || !sb->bc)
        return -EINVAL;

    ext4_inode_t inode;
    if (ext4_read_inode(sb, p->inode_num, &inode) < 0)
        return -EIO;

    uint64_t pages[EXT4_SYNC_MAX_PAGES];
    size_t n = 0;

    if (ext4_sync_extent_pages(sb, &inode, pages, &n, EXT4_SYNC_MAX_PAGES) < 0)
        goto full_sync;

    uint32_t inode_group = (p->inode_num - 1) / sb->inodes_per_group;
    if (ext4_sync_group_meta(sb, inode_group, pages, &n,
                             EXT4_SYNC_MAX_PAGES) < 0)
        goto full_sync;

    /* Block groups that hold the file's data extents: re-walk the leaf
     * extents (depth 0 covers the common case; a deep tree falls back below)
     * and add each extent's block group. */
    const uint8_t *raw = (const uint8_t *)&inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic == EXT4_EXT_MAGIC && hdr.eh_depth == 0) {
        int ne = hdr.eh_entries;
        if (ne > 4)
            ne = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), (size_t)ne * sizeof(ext4_extent_t));
        for (int i = 0; i < ne; i++) {
            uint16_t len = ext[i].ee_len;
            if (len > 0x8000)
                len -= 0x8000;
            if (!len)
                continue;
            uint64_t pb = (uint64_t)ext[i].ee_start_lo |
                          ((uint64_t)ext[i].ee_start_hi << 32);
            uint64_t blocks = (len + sb->blocks_per_group - 1) / sb->blocks_per_group;
            for (uint64_t g = pb / sb->blocks_per_group;
                 g < sb->groups_count && blocks > 0; g++, blocks--) {
                if (ext4_sync_group_meta(sb, (uint32_t)g, pages, &n,
                                         EXT4_SYNC_MAX_PAGES) < 0)
                    goto full_sync;
            }
        }
    }

    /* Superblock page: rarely dirty at runtime, but a zero-cost safety net
     * when the mount wrote feature/state updates. */
    if (sync_add_byte_range(pages, &n, EXT4_SYNC_MAX_PAGES, 0,
                            sb->block_size) < 0)
        goto full_sync;

    if (n == 0)
        return 0;

    /* Deduplicate and sort for bcache_sync_scoped()'s membership check. */
    for (size_t i = 1; i < n; i++) {
        uint64_t key = pages[i];
        size_t j = i;
        while (j > 0 && pages[j - 1] > key) {
            pages[j] = pages[j - 1];
            j--;
        }
        pages[j] = key;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m == 0 || pages[i] != pages[m - 1])
            pages[m++] = pages[i];
    }

    return bcache_sync_scoped(sb->bc, pages, m);

full_sync:
    return bcache_sync_checked(sb->bc);
}
