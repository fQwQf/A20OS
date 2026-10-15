#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/ext4_journal.h"
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
 * this file needs -- its data extents, the inode-record page holding its inode,
 * and the allocation metadata (bitmaps/group descriptors) of the block groups
 * it occupies -- and flush only those pages with bcache_sync_scoped().
 *
 * Correctness: a crash after fsync() must not lose the file's data, its inode,
 * or the bitmap marking its blocks allocated (a stale bitmap would let a later
 * allocation reuse blocks whose data was already made durable). Unjournaled
 * mounts use full checked sync because neighboring inode records can share a
 * cache page. Journaled scoped sync relies on the active journal hook. Other
 * files' unrelated data and metadata remain outside the selected set.
 */

#define EXT4_SYNC_MAX_PAGES 2048

#ifdef CONFIG_EXT4_RUST_EXTENT_TXN
typedef struct {
    uint64_t block_bitmap;
    uint64_t inode_bitmap;
    uint64_t inode_table;
    uint64_t descriptor_byte;
} ext4_sync_group_meta_t;

typedef struct {
    uint32_t block_size, blocks_per_group, groups_count;
    uint32_t inodes_per_group, inode_size, inode_number;
    uint64_t first_data_block, blocks_count, descriptor_table_byte;
    uint32_t descriptor_size, leaf_capacity, has_extents, has_journal_sync;
} ext4_sync_plan_input_t;

typedef struct {
    void *context;
    uint8_t *workspace;
    int (*read_block)(void *, uint64_t, uint8_t *, uint32_t);
    int (*group_meta)(void *, uint32_t, ext4_sync_group_meta_t *);
    int (*sync_scoped)(void *, const uint64_t *, size_t);
    int (*sync_full)(void *);
} ext4_sync_plan_ops_t;

_Static_assert(offsetof(ext4_sync_group_meta_t, descriptor_byte) == 24,
               "Rust ext4 sync group metadata ABI");
_Static_assert(sizeof(ext4_sync_group_meta_t) == 32,
               "Rust ext4 sync group metadata ABI size");
_Static_assert(offsetof(ext4_sync_plan_input_t, first_data_block) == 24,
               "Rust ext4 sync input ABI");
_Static_assert(offsetof(ext4_sync_plan_input_t, descriptor_table_byte) == 40,
               "Rust ext4 sync input ABI");
_Static_assert(offsetof(ext4_sync_plan_input_t, has_journal_sync) == 60,
               "Rust ext4 sync input ABI");
_Static_assert(sizeof(ext4_sync_plan_input_t) == 64,
               "Rust ext4 sync input ABI size");
_Static_assert(offsetof(ext4_sync_plan_ops_t, workspace) == sizeof(void *),
               "Rust ext4 sync ops ABI");
_Static_assert(offsetof(ext4_sync_plan_ops_t, read_block) == 2 * sizeof(void *),
               "Rust ext4 sync ops ABI");
_Static_assert(offsetof(ext4_sync_plan_ops_t, group_meta) == 3 * sizeof(void *),
               "Rust ext4 sync ops ABI");
_Static_assert(offsetof(ext4_sync_plan_ops_t, sync_scoped) == 4 * sizeof(void *),
               "Rust ext4 sync ops ABI");
_Static_assert(offsetof(ext4_sync_plan_ops_t, sync_full) == 5 * sizeof(void *),
               "Rust ext4 sync ops ABI");
_Static_assert(sizeof(ext4_sync_plan_ops_t) == 6 * sizeof(void *),
               "Rust ext4 sync ops ABI size");

extern int a20_ext4_sync_run(const ext4_sync_plan_input_t *,
                             const ext4_sync_plan_ops_t *, uint64_t *,
                             size_t, size_t *);

static int ext4_sync_plan_read_block(void *context, uint64_t block,
                                     uint8_t *out, uint32_t len)
{
    ext4_sb_info_t *sb = context;
    uint64_t offset;
    if (!sb || !sb->bc || !out || len != sb->block_size ||
        block >= sb->blocks_count ||
        __builtin_mul_overflow(block, (uint64_t)sb->block_size, &offset))
        return -EIO;
    /* A failed device read is not a harmless planner limitation: propagate
     * it so a successful full-cache flush cannot hide the failed read. */
    return bcache_read_bytes(sb->bc, offset, out, len) < 0 ? -EIO : 0;
}

static int ext4_sync_plan_group_meta(void *context, uint32_t group,
                                     ext4_sync_group_meta_t *out)
{
    ext4_sb_info_t *sb = context;
    uint64_t descriptor_offset;
    if (!sb || !out || !sb->group_descs || group >= sb->groups_count ||
        __builtin_mul_overflow((uint64_t)group, (uint64_t)sb->desc_size,
                               &descriptor_offset) ||
        __builtin_add_overflow(sb->block_group_desc_table_byte,
                               descriptor_offset, &descriptor_offset))
        return -EIO;
    const ext4_group_desc_t *gd = &sb->group_descs[group];
    out->block_bitmap = gd->bg_block_bitmap_lo;
    out->inode_bitmap = gd->bg_inode_bitmap_lo;
    out->inode_table = gd->bg_inode_table_lo;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        const uint8_t *hi = gd->bg_reserved0;
        out->block_bitmap |= ((uint64_t)hi[0] | ((uint64_t)hi[1] << 8) |
                             ((uint64_t)hi[2] << 16) | ((uint64_t)hi[3] << 24)) << 32;
        out->inode_bitmap |= ((uint64_t)hi[4] | ((uint64_t)hi[5] << 8) |
                             ((uint64_t)hi[6] << 16) | ((uint64_t)hi[7] << 24)) << 32;
        out->inode_table |= ((uint64_t)hi[8] | ((uint64_t)hi[9] << 8) |
                             ((uint64_t)hi[10] << 16) | ((uint64_t)hi[11] << 24)) << 32;
    }
    out->descriptor_byte = descriptor_offset;
    return 0;
}

static int ext4_sync_plan_flush_scoped(void *context, const uint64_t *pages,
                                       size_t count)
{
    ext4_sb_info_t *sb = context;
    if (!sb || !sb->bc)
        return -EIO;
    return bcache_sync_scoped(sb->bc, pages, count);
}

static int ext4_sync_plan_flush_full(void *context)
{
    ext4_sb_info_t *sb = context;
    if (!sb || !sb->bc)
        return -EIO;
    return bcache_sync_checked(sb->bc);
}

/* Enter journal commit through the cache's hook guard when that hook is the
 * active ext4 hook. Calling ext4_journal_commit() directly would make its
 * initial bcache_sync_checked() call the hook again while the outer commit
 * still holds a snapshot of the transaction. The nested commit can clear the
 * transaction, after which the outer commit would write that snapshot twice.
 */
static int ext4_sync_commit_before_plan(ext4_sb_info_t *sb)
{
    if (sb && sb->bc && sb->journal &&
        sb->bc->sync_hook == ext4_journal_sync_hook)
        return bcache_sync_checked(sb->bc);
    return ext4_journal_commit(sb);
}
#else

/* Append every 4 KiB page covering [byte_start, byte_start + byte_len).
 * Returns -1 when the bounded array is full (caller falls back to full sync). */
static int sync_add_byte_range(uint64_t *pages, size_t *n, size_t cap,
                               uint64_t byte_start, uint64_t byte_len)
{
    if (byte_len == 0)
        return 0;
    uint64_t byte_end;
    if (__builtin_add_overflow(byte_start, byte_len, &byte_end))
        return -EOVERFLOW;
    uint64_t first = byte_start / PCACHE_PAGE_SIZE;
    uint64_t last = (byte_end - 1) / PCACHE_PAGE_SIZE;
    for (uint64_t p = first;;) {
        if (*n >= cap)
            return -1;
        pages[(*n)++] = p;
        if (p == last)
            break;
        if (__builtin_add_overflow(p, 1ULL, &p))
            return -EOVERFLOW;
    }
    return 0;
}

static int ext4_sync_group_meta(ext4_sb_info_t *sb, uint32_t group,
                                uint32_t inode_number, uint64_t *pages,
                                size_t *n, size_t cap);
static int ext4_sync_add_data_extent(ext4_sb_info_t *sb, uint64_t physical,
                                     uint64_t length, uint64_t *pages,
                                     size_t *n, size_t cap);

/* Recursively collect the data pages of a depth-0/1 extent tree plus the
 * index blocks themselves.  Deeper trees are extremely rare for build files;
 * a depth > 1 signals the caller to fall back to a full mount sync. */
static int ext4_sync_extent_pages(ext4_sb_info_t *sb, const ext4_inode_t *inode,
                                  uint64_t *pages, size_t *n, size_t cap)
{
    const uint8_t *raw = (const uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_max == 0 ||
        hdr.eh_max > 4 || hdr.eh_entries > hdr.eh_max ||
        sizeof(hdr) + (size_t)hdr.eh_max * sizeof(ext4_extent_t) > 60)
        return -EIO;
    if (hdr.eh_entries == 0)
        return 0;
    /* The C reference deliberately uses a full checked sync for non-root
     * trees. Rust owns the bounded depth-1 parser in the opt-in profile. */
    if (hdr.eh_depth != 0)
        return -1;
    if (hdr.eh_depth == 0) {
        int ne = hdr.eh_entries;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), (size_t)ne * sizeof(ext4_extent_t));
        for (int i = 0; i < ne; i++) {
            uint16_t len = ext[i].ee_len;
            if (len > 0x8000)
                len -= 0x8000;
            if (!len)
                return -EIO;
            uint64_t pb = (uint64_t)ext[i].ee_start_lo |
                          ((uint64_t)ext[i].ee_start_hi << 32);
            int ret = ext4_sync_add_data_extent(sb, pb, len, pages, n, cap);
            if (ret < 0)
                return ret;
        }
        return 0;
    }
    if (hdr.eh_depth > 1)
        return -1;

    int ni = hdr.eh_entries;
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
        if (eh.eh_magic != EXT4_EXT_MAGIC || eh.eh_depth != 0 ||
            eh.eh_max == 0 ||
            eh.eh_max > (sb->block_size - sizeof(eh)) / sizeof(ext4_extent_t) ||
            eh.eh_entries > eh.eh_max) {
            kfree(b);
            return -EIO;
        }
        if (eh.eh_entries != 0) {
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
                                uint32_t inode_number, uint64_t *pages,
                                size_t *n, size_t cap)
{
    if (group >= sb->groups_count)
        return -EIO;
    const ext4_group_desc_t *gd = &sb->group_descs[group];
    uint64_t bbm = (uint64_t)gd->bg_block_bitmap_lo;
    uint64_t ibm = (uint64_t)gd->bg_inode_bitmap_lo;
    uint64_t it = (uint64_t)gd->bg_inode_table_lo;
    if (sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
        const uint8_t *hi = gd->bg_reserved0;
        bbm |= ((uint64_t)hi[0] | ((uint64_t)hi[1] << 8) |
                ((uint64_t)hi[2] << 16) | ((uint64_t)hi[3] << 24)) << 32;
        ibm |= ((uint64_t)hi[4] | ((uint64_t)hi[5] << 8) |
                ((uint64_t)hi[6] << 16) | ((uint64_t)hi[7] << 24)) << 32;
        it |= ((uint64_t)hi[8] | ((uint64_t)hi[9] << 8) |
               ((uint64_t)hi[10] << 16) | ((uint64_t)hi[11] << 24)) << 32;
    }
    uint64_t inode_bytes, table_blocks, gdt_delta, gdt_byte;
    uint64_t bbm_byte, ibm_byte, it_byte, table_end;
    if (!sb->block_size || !sb->inodes_per_group || !sb->inode_size)
        return -EIO;
    if (__builtin_mul_overflow((uint64_t)sb->inodes_per_group,
                               (uint64_t)sb->inode_size, &inode_bytes) ||
        __builtin_add_overflow(inode_bytes, (uint64_t)sb->block_size - 1,
                               &table_blocks) ||
        __builtin_mul_overflow((uint64_t)group, (uint64_t)sb->desc_size,
                               &gdt_delta) ||
        __builtin_add_overflow(sb->block_group_desc_table_byte, gdt_delta,
                               &gdt_byte) ||
        __builtin_mul_overflow(bbm, (uint64_t)sb->block_size, &bbm_byte) ||
        __builtin_mul_overflow(ibm, (uint64_t)sb->block_size, &ibm_byte) ||
        __builtin_mul_overflow(it, (uint64_t)sb->block_size, &it_byte))
        return -EOVERFLOW;
    table_blocks /= sb->block_size;
    if (__builtin_add_overflow(it, table_blocks, &table_end) ||
        bbm >= sb->blocks_count || ibm >= sb->blocks_count ||
        table_end > sb->blocks_count)
        return -EIO;
    if (sync_add_byte_range(pages, n, cap, bbm_byte,
                            sb->block_size) < 0 ||
        sync_add_byte_range(pages, n, cap, ibm_byte,
                            sb->block_size) < 0 ||
        sync_add_byte_range(pages, n, cap, gdt_byte, sb->desc_size) < 0)
        return -1;
    if (inode_number != 0) {
        if (inode_number - 1 >= sb->inodes_count)
            return -EIO;
        uint64_t inode_index = (inode_number - 1) % sb->inodes_per_group;
        uint64_t inode_offset, inode_byte, inode_end;
        if (__builtin_mul_overflow(inode_index, (uint64_t)sb->inode_size,
                                   &inode_offset) ||
            __builtin_add_overflow(it_byte, inode_offset, &inode_byte) ||
            __builtin_add_overflow(inode_offset, (uint64_t)sb->inode_size,
                                   &inode_end))
            return -EOVERFLOW;
        if (inode_end > inode_bytes)
            return -EIO;
        int ret = sync_add_byte_range(pages, n, cap, inode_byte,
                                      sb->inode_size);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int ext4_sync_add_data_extent(ext4_sb_info_t *sb, uint64_t physical,
                                     uint64_t length, uint64_t *pages,
                                     size_t *n, size_t cap)
{
    uint64_t end, byte_start, byte_len;
    if (!length || !sb->blocks_per_group || physical < sb->first_data_block ||
        __builtin_add_overflow(physical, length, &end) || end > sb->blocks_count ||
        __builtin_mul_overflow(physical, (uint64_t)sb->block_size, &byte_start) ||
        __builtin_mul_overflow(length, (uint64_t)sb->block_size, &byte_len))
        return -EIO;
    int ret = sync_add_byte_range(pages, n, cap, byte_start, byte_len);
    if (ret < 0)
        return ret;
    uint64_t bpg = sb->blocks_per_group;
    uint64_t first_group = (physical - sb->first_data_block) / bpg;
    uint64_t last_group = (end - 1 - sb->first_data_block) / bpg;
    if (last_group >= sb->groups_count)
        return -EIO;
    for (uint64_t group = first_group; group <= last_group; group++) {
        ret = ext4_sync_group_meta(sb, (uint32_t)group, 0, pages, n, cap);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int ext4_sync_commit_before_plan(ext4_sb_info_t *sb)
{
    if (sb && sb->bc && sb->journal &&
        sb->bc->sync_hook == ext4_journal_sync_hook)
        return bcache_sync_checked(sb->bc);
    return ext4_journal_commit(sb);
}
#endif

int ext4_vn_sync(vnode_t *vn)
{
    if (!vn || !vn->fs_data)
        return -EINVAL;
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_sb_info_t *sb = p->sb;
    if (!sb || !sb->bc)
        return -EINVAL;
    if (ext4_is_inconsistent(sb))
        return -117;

    /* Commit the open transaction first, and report its failure to fsync()
     * rather than letting a later writeback hide it.  The block cache's
     * pre-sync hook does the same for the generic sync paths; doing it
     * explicitly here keeps the ordering visible at the call site and means
     * the page set collected below is only advisory -- the metadata it names
     * is already durable once this returns. */
    int commit_ret = ext4_sync_commit_before_plan(sb);
    if (commit_ret < 0)
        return commit_ret;

    ext4_inode_t inode;
    if (ext4_read_inode(sb, p->inode_num, &inode) < 0)
        return -EIO;

#ifdef CONFIG_EXT4_RUST_EXTENT_TXN
    /* Rust forms a mutable slice of the full capacity, so every u64 must be
     * initialized before entering the FFI. */
    uint64_t *pages = kcalloc(EXT4_SYNC_MAX_PAGES, sizeof(*pages));
    uint8_t *workspace = kmalloc(sb->block_size);
    if (!pages || !workspace || sb->block_size < 60) {
        if (pages) kfree(pages);
        if (workspace) kfree(workspace);
        return bcache_sync_checked(sb->bc);
    }
    memcpy(workspace, (const uint8_t *)&inode + offsetof(ext4_inode_t, i_block),
           60);
    uint32_t leaf_capacity = sb->block_size >= 16
        ? (sb->block_size - 12 -
           ((sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) ? 4 : 0)) / 12
        : 0;
    ext4_sync_plan_input_t input = {
        .block_size = sb->block_size,
        .blocks_per_group = sb->blocks_per_group,
        .groups_count = sb->groups_count,
        .inodes_per_group = sb->inodes_per_group,
        .inode_size = sb->inode_size,
        .inode_number = p->inode_num,
        .first_data_block = sb->first_data_block,
        .blocks_count = sb->blocks_count,
        .descriptor_table_byte = sb->block_group_desc_table_byte,
        .descriptor_size = sb->desc_size,
        .leaf_capacity = leaf_capacity,
        .has_extents = (inode.i_flags & EXT4_EXTENTS_FL) != 0,
        .has_journal_sync = sb->journal != NULL &&
                            sb->bc->sync_hook == ext4_journal_sync_hook,
    };
    ext4_sync_plan_ops_t ops = {
        .context = sb,
        .workspace = workspace,
        .read_block = ext4_sync_plan_read_block,
        .group_meta = ext4_sync_plan_group_meta,
        .sync_scoped = ext4_sync_plan_flush_scoped,
        .sync_full = ext4_sync_plan_flush_full,
    };
    size_t n = 0;
    int ret = a20_ext4_sync_run(&input, &ops, pages,
                                EXT4_SYNC_MAX_PAGES, &n);
    kfree(workspace);
    kfree(pages);
    return ret;
#else
    /* A journal pointer alone does not establish the ordering contract: the
     * C UFSD write-through adapter has no effective sync hook/holds. In that
     * profile use checked whole-cache sync; kernel and Rust-ABI caches install
     * the ext4 commit hook and may use the scoped planner. */
    if (!sb->journal || sb->bc->sync_hook != ext4_journal_sync_hook)
        return bcache_sync_checked(sb->bc);

    uint64_t *pages = kcalloc(EXT4_SYNC_MAX_PAGES, sizeof(*pages));
    if (!pages)
        return bcache_sync_checked(sb->bc);
    size_t n = 0;

    int extent_status = (inode.i_flags & EXT4_EXTENTS_FL)
        ? ext4_sync_extent_pages(sb, &inode, pages, &n, EXT4_SYNC_MAX_PAGES)
        : -1;
    if (extent_status == -EIO || extent_status == -EOVERFLOW) {
        kfree(pages);
        return extent_status;
    }
    if (extent_status < 0)
        goto full_sync;

    uint32_t inode_group = (p->inode_num - 1) / sb->inodes_per_group;
    int group_ret = ext4_sync_group_meta(sb, inode_group, p->inode_num, pages, &n,
                                         EXT4_SYNC_MAX_PAGES);
    if (group_ret == -EIO || group_ret == -EOVERFLOW) {
        kfree(pages);
        return group_ret;
    }
    if (group_ret < 0)
        goto full_sync;

    /* Superblock page: rarely dirty at runtime, but a zero-cost safety net
     * when the mount wrote feature/state updates. */
    if (sync_add_byte_range(pages, &n, EXT4_SYNC_MAX_PAGES, 0,
                            sb->block_size) < 0)
        goto full_sync;

    if (n == 0) {
        kfree(pages);
        return 0;
    }

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

    int sync_ret = bcache_sync_scoped(sb->bc, pages, m);
    kfree(pages);
    return sync_ret;

full_sync:
    {
        int sync_ret = bcache_sync_checked(sb->bc);
        kfree(pages);
        return sync_ret;
    }
#endif
}
