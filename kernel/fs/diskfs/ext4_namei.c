#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/ext4_journal.h"
#include "fs/page_cache.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timekeeping.h"
#include "proc/proc.h"

/* ext4 namei: directory entries and vnode operations. */

#ifdef CONFIG_EXT4_RUST_EXTENT_TXN
/* Defined in ext4.c; the Rust-owned partial-truncate transaction behind the
 * extent-tree path of truncate(). */
int ext4_extent_partial_truncate_checked(ext4_sb_info_t *sb, uint32_t ino,
                                         ext4_inode_t *inode, uint32_t lblk,
                                         uint64_t new_size);
#endif

int ext4_dir_entry_check(const ext4_dir_entry_t *de, uint32_t off,
                         uint32_t block_size, uint16_t *actual_len)
{
    if (off > block_size || block_size - off < 8)
        return -EIO;
    uint16_t rec_len = de->rec_len;
    if (rec_len < 8 || (rec_len & 3U) || rec_len > block_size - off ||
        de->name_len > rec_len - 8U)
        return -EIO;
    uint16_t actual = (uint16_t)((8U + de->name_len + 3U) & ~3U);
    if (actual > rec_len)
        return -EIO;
    if (actual_len)
        *actual_len = actual;
    return 0;
}

/* A checksummed directory block ends in a 12-byte tail, so an insertion has to
 * leave that much room rather than filling the block to its last byte. */
static size_t ext4_dir_tail_reserve(const ext4_sb_info_t *sb)
{
    return ext4_has_metadata_csum(sb) ? 12 : 0;
}

static int ext4_dir_write_block(ext4_sb_info_t *sb, uint32_t dir_ino,
                                uint32_t dir_gen, uint64_t physical,
                                void *block)
{
    /* A directory block is part of the namespace, not file data: ext4 logs
     * it so that a crash cannot leave a half-added or half-removed entry. */
    uint8_t *raw = (uint8_t *)block;
    if (ext4_has_metadata_csum(sb)) {
        /* Re-terminate and re-checksum on every write.  The tail was already
         * there for a block read off disk; installing it again is harmless
         * because a block the kernel built itself never had one. */
        ext4_dir_block_tail_set(raw, sb->block_size);
        ext4_dir_block_checksum_put(sb, dir_ino, dir_gen, raw, sb->block_size);
    }
    return ext4_meta_write(sb, physical * sb->block_size, raw,
                           sb->block_size) < 0 ? -EIO : 0;
}

/* Readers walk every directory block linearly. Before a mutation, convert
 * an imported HTree root (and any empty interior index blocks) to that same
 * representation. Treating '..' padding as free space while INDEX_FL stays
 * set overwrites dx_root and leaves a corrupt directory for Linux/e2fsck. */
static int ext4_dir_make_linear(ext4_sb_info_t *sb, ext4_inode_t *di,
                                uint32_t dir_ino, uint64_t size)
{
    if (!(di->i_flags & EXT4_INDEX_FL)) return 0;
    uint32_t bs = sb->block_size;
    uint32_t blocks = (uint32_t)((size + bs - 1) / bs);
    size_t reserve = ext4_dir_tail_reserve(sb);
    char *raw = kmalloc(bs);
    if (!raw) return -ENOMEM;
    int ret = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        uint64_t physical = ext4_block_map(sb, di, b);
        if (!physical || bcache_read_bytes(sb->bc, physical * bs, raw, bs) < 0) {
            ret = -EIO; break;
        }
        ext4_dir_entry_t *first = (ext4_dir_entry_t *)raw;
        if (b == 0) {
            if (ext4_dir_entry_check(first, 0, bs, NULL) < 0 ||
                first->name_len != 1 || first->name[0] != '.') {
                ret = -EIO; break;
            }
            uint32_t off = first->rec_len;
            ext4_dir_entry_t *parent = (ext4_dir_entry_t *)(raw + off);
            if (ext4_dir_entry_check(parent, off, bs, NULL) < 0 ||
                parent->name_len != 2 || memcmp(parent->name, "..", 2) ||
                off != 12 || parent->rec_len != bs - off) {
                ret = -EIO; break;
            }
            uint32_t parent_ino = parent->inode;
            memset(raw, 0, bs);
            first->inode = dir_ino; first->rec_len = 12;
            first->name_len = 1; first->file_type = EXT4_FT_DIR;
            first->name[0] = '.';
            parent = (ext4_dir_entry_t *)(raw + 12);
            parent->inode = parent_ino; parent->rec_len = bs - 12 - reserve;
            parent->name_len = 2; parent->file_type = EXT4_FT_DIR;
            memcpy(parent->name, "..", 2);
        } else if (first->inode == 0 && first->rec_len == bs) {
            /* dx_node masquerades as one unused full-block dirent. */
            memset(raw, 0, bs);
            first->rec_len = bs - reserve;
        } else {
            continue; /* Ordinary leaf entries already use linear layout. */
        }
        ret = ext4_dir_write_block(sb, dir_ino, di->i_generation, physical, raw);
        if (ret < 0) break;
    }
    kfree(raw);
    if (ret == 0) {
        di->i_flags &= ~EXT4_INDEX_FL;
        ret = ext4_write_inode(sb, dir_ino, di);
    }
    if (ret < 0) ext4_mark_inconsistent(sb);
    return ret;
}

int ext4_dir_find(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz,
                          const char *name, uint32_t *out_ino, uint8_t *out_ft) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    /* One scratch buffer reused for every block of the directory: a per-block
     * kmalloc here is on the shared slab lock that eight parallel rustc
     * processes all pay on each cold directory miss. */
    char *blk = (char *)kmalloc(bs);
    if (!blk) return -ENOMEM;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->file_type == EXT4_FT_DIRENT_TAIL) break;
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (out_ino) *out_ino = de->inode;
                if (out_ft) *out_ft = de->file_type;
                kfree(blk); return 0;
            }
            off += de->rec_len;
        }
    }
    kfree(blk);
    return -ENOENT;
}


int ext4_dir_add(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dir_ino,
                         uint64_t *dsz, const char *name, uint32_t ino,
                         uint8_t ft) {
    uint32_t bs = sb->block_size;
    size_t nl = strlen(name);
    if (nl == 0) return -EINVAL;
    if (nl > 255) return -ENAMETOOLONG;
    uint16_t need = (uint16_t)((8 + nl + 3) & ~3);
    size_t reserve = ext4_dir_tail_reserve(sb);
    if (need + reserve > bs) return -ENAMETOOLONG;
    int linear = ext4_dir_make_linear(sb, di, dir_ino, *dsz);
    if (linear < 0) return linear;
    uint32_t nb = (*dsz + bs - 1) / bs;

    char *blk = (char *)kmalloc(bs);
    if (!blk) return -ENOMEM;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            uint16_t actual;
            if (ext4_dir_entry_check(de, off, bs, &actual) < 0) {
                kfree(blk); return -EIO;
            }
            /* The block's trailing dirent is the terminator, not free space:
             * writing into it puts the entry exactly where the tail is about
             * to be reinstalled, so the entry vanishes on the next write and
             * the caller is told the name was added. */
            if (de->file_type == EXT4_FT_DIRENT_TAIL) break;
            if (de->inode == 0 && de->rec_len >= need) {
                uint16_t old = de->rec_len;
                uint16_t left = old - need;
                de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
                memcpy(de->name, name, nl);
                /* Whatever follows the new entry is a directory entry of its
                 * own and needs a complete eight-byte header, so a filler
                 * with less room than that is absorbed whole. */
                if (left >= 8) {
                    de->rec_len = need;
                    ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + need);
                    nx->inode = 0; nx->rec_len = left; nx->name_len = 0; nx->file_type = 0;
                } else {
                    de->rec_len = old;
                }
                int wr = ext4_dir_write_block(sb, dir_ino, di->i_generation, p, blk);
                kfree(blk); return wr;
            }
            if (de->file_type == EXT4_FT_DIRENT_TAIL) break;
            uint16_t slack = de->rec_len - actual;
            if (slack >= need + reserve) {
                uint16_t old = de->rec_len; de->rec_len = actual;
                ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + actual);
                nx->inode = ino; nx->rec_len = old - actual;
                nx->name_len = (uint8_t)nl; nx->file_type = ft;
                memcpy(nx->name, name, nl);
                int wr = ext4_dir_write_block(sb, dir_ino, di->i_generation, p, blk);
                kfree(blk); return wr;
            }
            off += de->rec_len;
        }
    }
    uint64_t nb_blk = ext4_alloc_block(sb); if (!nb_blk) { kfree(blk); return -ENOSPC; }
    int gr = ext4_block_grow(sb, dir_ino, di, nb, nb_blk);
    if (gr < 0) { ext4_free_block(sb, nb_blk); kfree(blk); return gr; }
    memset(blk, 0, bs);
    ext4_dir_entry_t *de = (ext4_dir_entry_t *)blk;
    de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
    memcpy(de->name, name, nl);
    if (bs - need - reserve >= 8) {
        de->rec_len = need;
        ext4_dir_entry_t *tail = (ext4_dir_entry_t *)(blk + need);
        tail->inode = 0; tail->rec_len = (uint16_t)(bs - need - reserve);
        tail->name_len = 0;
    } else de->rec_len = (uint16_t)bs;
    int wr = ext4_dir_write_block(sb, dir_ino, di->i_generation, nb_blk, blk);
    kfree(blk);
    if (wr < 0) return wr;
    *dsz += bs;
    return 0;
}


int ext4_dir_remove(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dir_ino,
                            uint64_t dsz, const char *name) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    int linear = ext4_dir_make_linear(sb, di, dir_ino, dsz);
    if (linear < 0) return linear;
    char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0, prev = 0; int hp = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->file_type == EXT4_FT_DIRENT_TAIL) break;
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (hp) ((ext4_dir_entry_t *)(blk + prev))->rec_len += de->rec_len;
                else de->inode = 0;
                int wr = ext4_dir_write_block(sb, dir_ino, di->i_generation, p, blk);
                kfree(blk); return wr;
            }
            prev = off; hp = 1; off += de->rec_len;
        }
    }
    kfree(blk);
    return -ENOENT;
}


int ext4_dir_update_entry(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dir_ino,
                                  uint64_t *dsz, const char *name, uint32_t ino,
                                  uint8_t ft) {
    uint32_t bs = sb->block_size;
    size_t nl = strlen(name);
    if (nl > 255) return -ENAMETOOLONG;
    int linear = ext4_dir_make_linear(sb, di, dir_ino, *dsz);
    if (linear < 0) return linear;
    uint32_t nb = (*dsz + bs - 1) / bs;
    char *blk = (char *)kmalloc(bs);
    if (!blk) return -ENOMEM;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b);
        if (!p) continue;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->file_type == EXT4_FT_DIRENT_TAIL) break;
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                de->inode = ino;
                de->file_type = ft;
                int wr = ext4_dir_write_block(sb, dir_ino, di->i_generation, p, blk);
                kfree(blk);
                return wr;
            }
            off += de->rec_len;
        }
    }
    kfree(blk);
    return -ENOENT;
}


/* Clear an inode for reuse and stamp the deletion time on it.  i_dtime is a
 * timestamp, not a flag: e2fsck reads any value below the inode count as the
 * ext2 "fast delete" marker -- an inode whose blocks were released without
 * being cleared first -- and answers "Inodes that were part of a corrupted
 * orphan linked list found" for every inode left that way, which turns an
 * otherwise perfectly healthy image into a filesystem with errors. */
static void ext4_inode_mark_deleted(ext4_inode_t *inode)
{
    uint64_t now[2];
    timekeeping_get_realtime(now);
    memset(inode, 0, sizeof(*inode));
    inode->i_dtime = (uint32_t)now[0];
}

typedef struct {
    void *context;
    uint32_t inode_count;
    uint32_t first_ino;
    int (*read_head)(void *, uint32_t *);
    int (*write_head)(void *, uint32_t);
    int (*read_inode)(void *, uint32_t, uint16_t *, uint32_t *);
    int (*write_inode)(void *, uint32_t, uint16_t, uint32_t);
    int (*truncate_linked_inode)(void *, uint32_t);
    int (*truncate_unlinked_inode)(void *, uint32_t);
    int (*release_unlinked_inode)(void *, uint32_t);
    void (*abort_transaction)(void *);
} ext4_orphan_ops_t;

#ifdef CONFIG_UFSD_RUST_EXT4_ORPHAN
extern int a20_ext4_orphan_add_last_link(const ext4_orphan_ops_t *, uint32_t);
extern int a20_ext4_orphan_reap(const ext4_orphan_ops_t *, uint32_t);
extern int a20_ext4_orphan_recover(const ext4_orphan_ops_t *, uint32_t *);
#endif

static int ext4_orphan_read_head(void *ctx, uint32_t *out)
{
    ext4_sb_info_t *sb = ctx;
    if (!sb || !out) return -EINVAL;
    uint8_t le[4];
    if (bcache_read_bytes(sb->bc, EXT4_SB_OFFSET +
        offsetof(ext4_superblock_t, s_last_orphan), le, sizeof(le)) < 0)
        return -EIO;
    *out = (uint32_t)le[0] | ((uint32_t)le[1] << 8) |
           ((uint32_t)le[2] << 16) | ((uint32_t)le[3] << 24);
    return 0;
}

static int ext4_orphan_write_head(void *ctx, uint32_t ino)
{
    return ext4_superblock_set_last_orphan(ctx, ino);
}

static int ext4_orphan_read_inode(void *ctx, uint32_t ino, uint16_t *links,
                                  uint32_t *next)
{
    ext4_inode_t inode;
    ext4_sb_info_t *sb = ctx;
    if (!sb || !links || !next || ext4_read_inode(sb, ino, &inode) < 0)
        return -EIO;
    *links = inode.i_links_count;
    *next = inode.i_dtime;
    return 0;
}

static int ext4_orphan_write_inode(void *ctx, uint32_t ino, uint16_t links,
                                   uint32_t next)
{
    ext4_sb_info_t *sb = ctx;
    ext4_inode_t inode;
    int ret = ext4_read_inode(sb, ino, &inode);
    if (ret < 0) return ret;
    inode.i_links_count = links;
    inode.i_dtime = next;
    return ext4_write_inode(sb, ino, &inode);
}

static int ext4_orphan_truncate_linked_inode(void *ctx, uint32_t ino)
{
    ext4_sb_info_t *sb = ctx;
    ext4_inode_t inode;
    int ret = ext4_read_inode(sb, ino, &inode);
    if (ret < 0) return ret;
    if (inode.i_links_count == 0) return -EIO;
    uint64_t size = ext4_inode_size(&inode);
    uint64_t cutoff = size / sb->block_size + (size % sb->block_size != 0);
    if (cutoff > 0xffffffffULL) return -EFBIG;
    ret = ext4_block_truncate_at_checked(sb, ino, &inode, (uint32_t)cutoff);
    if (ret < 0) {
        if (sb->journal) (void)ext4_journal_poison(sb);
        return ret;
    }
    return 0;
}

static int ext4_orphan_truncate_unlinked_inode(void *ctx, uint32_t ino)
{
    ext4_sb_info_t *sb = ctx;
    ext4_inode_t inode;
    int ret = ext4_read_inode(sb, ino, &inode);
    if (ret < 0) return ret;
    if (inode.i_links_count != 0) return -EIO;
    ret = ext4_block_truncate_checked(sb, ino, &inode);
    if (ret < 0) {
        printf("[EXT4] orphan truncate failed ino=%u rc=%d mode=%x flags=%x size=%llu blocks=%u\n",
               ino, ret, inode.i_mode, inode.i_flags,
               (unsigned long long)ext4_inode_size(&inode), inode.i_blocks_lo);
        if (sb->journal) (void)ext4_journal_poison(sb);
        return ret;
    }
    return 0;
}

static int ext4_orphan_release_unlinked_inode(void *ctx, uint32_t ino)
{
    ext4_sb_info_t *sb = ctx;
    ext4_inode_t inode;
    int ret = ext4_read_inode(sb, ino, &inode);
    if (ret < 0) return ret;
    if (inode.i_links_count != 0) return -EIO;
    ext4_inode_mark_deleted(&inode);
    ret = ext4_write_inode(sb, ino, &inode);
    if (ret == 0) ret = ext4_free_inode(sb, ino);
    if (ret < 0 && sb->journal) (void)ext4_journal_poison(sb);
    return ret;
}

static void ext4_orphan_abort_transaction(void *ctx)
{
    ext4_sb_info_t *sb = ctx;
    if (sb && sb->journal) (void)ext4_journal_poison(sb);
}

static ext4_orphan_ops_t ext4_orphan_ops(ext4_sb_info_t *sb)
{
    ext4_orphan_ops_t ops = {
        .context = sb,
        .inode_count = sb ? sb->inodes_count : 0,
        .first_ino = sb ? sb->first_ino : 0,
        .read_head = ext4_orphan_read_head,
        .write_head = ext4_orphan_write_head,
        .read_inode = ext4_orphan_read_inode,
        .write_inode = ext4_orphan_write_inode,
        .truncate_linked_inode = ext4_orphan_truncate_linked_inode,
        .truncate_unlinked_inode = ext4_orphan_truncate_unlinked_inode,
        .release_unlinked_inode = ext4_orphan_release_unlinked_inode,
        .abort_transaction = ext4_orphan_abort_transaction,
    };
    return ops;
}

static int ext4_orphan_add_last_link(ext4_sb_info_t *sb, uint32_t ino);
static int ext4_orphan_reap(ext4_sb_info_t *sb, uint32_t ino);

/* C-profile reference state machine. The Rust profile dispatches the identical
 * narrow callbacks through ext4_orphan.rs and owns validation/order there. */
#ifndef CONFIG_UFSD_RUST_EXT4_ORPHAN
static int ext4_orphan_chain_contains(ext4_orphan_ops_t *ops, uint32_t target)
{
    uint32_t cur = 0, steps = 0;
    if (ops->read_head(ops->context, &cur) < 0) return -EIO;
    while (cur) {
        if (cur < ops->first_ino || cur > ops->inode_count ||
            steps++ >= ops->inode_count)
            return -EIO;
        if (cur == target) return -EEXIST;
        uint16_t links;
        uint32_t next;
        if (ops->read_inode(ops->context, cur, &links, &next) < 0) return -EIO;
        (void)links;
        cur = next;
    }
    return 0;
}
#endif

static int ext4_orphan_add_last_link(ext4_sb_info_t *sb, uint32_t ino)
{
    ext4_orphan_ops_t ops = ext4_orphan_ops(sb);
#ifdef CONFIG_UFSD_RUST_EXT4_ORPHAN
    return ext4_record_status(sb, a20_ext4_orphan_add_last_link(&ops, ino));
#else
    int ret = ext4_orphan_chain_contains(&ops, ino);
    if (ret < 0) return ret;
    uint16_t links;
    uint32_t old_next, head;
    if (ops.read_inode(ops.context, ino, &links, &old_next) < 0 || links != 1)
        return -EIO;
    if (ops.read_head(ops.context, &head) < 0) return -EIO;
    ret = ops.write_inode(ops.context, ino, 0, head);
    if (ret < 0) {
        int restore = ops.write_inode(ops.context, ino, links, old_next);
        ops.abort_transaction(ops.context);
        return restore < 0 ? ext4_record_status(sb, -117) : ret;
    }
    ret = ops.write_head(ops.context, ino);
    if (ret < 0) {
        int restore = ops.write_inode(ops.context, ino, links, old_next);
        if (restore == 0) restore = ops.write_head(ops.context, head);
        ops.abort_transaction(ops.context);
        return restore < 0 ? ext4_record_status(sb, -117) : ret;
    }
    return ret;
#endif
}

static int ext4_orphan_reap(ext4_sb_info_t *sb, uint32_t ino)
{
    ext4_orphan_ops_t ops = ext4_orphan_ops(sb);
#ifdef CONFIG_UFSD_RUST_EXT4_ORPHAN
    return ext4_record_status(sb, a20_ext4_orphan_reap(&ops, ino));
#else
    uint32_t prev = 0, cur = 0, steps = 0, found_prev = 0;
    uint16_t found_links = 0;
    uint32_t found_next = 0;
    int found = 0;
    if (ops.read_head(ops.context, &cur) < 0) return -EIO;
    while (cur) {
        if (cur < ops.first_ino || cur > ops.inode_count ||
            steps++ >= ops.inode_count) {
            printf("[EXT4] orphan reap preflight invalid target=%u current=%u previous=%u steps=%u\n",
                   ino, cur, prev, steps);
            return -EIO;
        }
        uint16_t links;
        uint32_t next;
        if (ops.read_inode(ops.context, cur, &links, &next) < 0) {
            printf("[EXT4] orphan reap preflight read failed target=%u current=%u previous=%u\n",
                   ino, cur, prev);
            return -EIO;
        }
        if (cur == ino) {
            if (found) return -EIO;
            found = 1;
            found_prev = prev;
            found_links = links;
            found_next = next;
        }
        prev = cur;
        cur = next;
    }
    if (!found) {
        printf("[EXT4] orphan reap preflight missing target=%u steps=%u\n", ino, steps);
        return -EIO;
    }
    int ret = found_links ? ops.truncate_linked_inode(ops.context, ino)
                          : ops.truncate_unlinked_inode(ops.context, ino);
    if (ret < 0) { ops.abort_transaction(ops.context); return ret; }
    uint16_t prev_links = 0;
    uint32_t ignored = 0;
    if (found_prev && ops.read_inode(ops.context, found_prev,
                                     &prev_links, &ignored) < 0) {
        ops.abort_transaction(ops.context);
        return -EIO;
    }
    if (!found_prev) {
        ret = ops.write_head(ops.context, found_next);
    } else {
        ret = ops.write_inode(ops.context, found_prev, prev_links, found_next);
    }
    if (ret < 0) {
        int restore = ops.write_inode(ops.context, ino, found_links, found_next);
        if (restore == 0) {
            if (!found_prev)
                restore = ops.write_head(ops.context, ino);
            else
                restore = ops.write_inode(ops.context, found_prev,
                                          prev_links, ino);
        }
        ops.abort_transaction(ops.context);
        return restore < 0 ? ext4_record_status(sb, -117) : ret;
    }
    if (found_links == 0) {
        ret = ops.release_unlinked_inode(ops.context, ino);
        if (ret < 0) {
            if (ret == -117) {
                ops.abort_transaction(ops.context);
                return ret;
            }
            int restore = ops.write_inode(ops.context, ino, found_links, found_next);
            if (restore == 0) {
                if (!found_prev)
                    restore = ops.write_head(ops.context, ino);
                else
                    restore = ops.write_inode(ops.context, found_prev,
                                              prev_links, ino);
            }
            ops.abort_transaction(ops.context);
            return restore < 0 ? ext4_record_status(sb, -117) : ret;
        }
    }
    return ret;
#endif
}

int ext4_orphan_recover(ext4_sb_info_t *sb)
{
    ext4_orphan_ops_t ops = ext4_orphan_ops(sb);
#ifdef CONFIG_UFSD_RUST_EXT4_ORPHAN
    uint32_t recovered = 0;
    return ext4_record_status(sb,
        a20_ext4_orphan_recover(&ops, &recovered));
#else
    uint32_t cur = 0, steps = 0;
    if (ops.read_head(ops.context, &cur) < 0) return -EIO;
    /* Preflight the entire chain before modifying any inode or bitmap. */
    uint32_t scan = cur;
    while (scan) {
        if (scan < ops.first_ino || scan > ops.inode_count ||
            steps++ >= ops.inode_count) return -EIO;
        uint16_t links;
        uint32_t next;
        if (ops.read_inode(ops.context, scan, &links, &next) < 0) return -EIO;
        scan = next;
    }
    /* Reap_one validates the full chain itself; do it before each node so a
     * corrupt tail never causes a prefix to be reclaimed. */
    steps = 0;
    while (cur) {
        if (steps++ >= ops.inode_count) return -EIO;
        int ret = ext4_orphan_reap(sb, cur);
        if (ret < 0) return ret;
        if (ops.read_head(ops.context, &cur) < 0) return -EIO;
    }
    return 0;
#endif
}

int ext4_inode_remove(ext4_sb_info_t *sb, mount_t *mnt,
                               uint32_t dir_ino __attribute__((unused)),
                               ext4_inode_t *di, const char *name, uint32_t ino,
                               vnode_t **deferred_put) {
    /* Read before removing the name so unsupported no-journal last-link
     * removal can fail without changing the directory. */
    ext4_inode_t victim;
    int r = ext4_read_inode(sb, ino, &victim);
    if (r < 0) return r;

    /* Drop one link.  Hard-linked files survive until the last link is gone. */
    if (victim.i_links_count > 1) {
        r = ext4_dir_remove(sb, di, dir_ino, ext4_inode_size(di), name);
        if (r < 0) return r;
        victim.i_links_count--;
        uint64_t now[2];
        timekeeping_get_realtime(now);
        victim.i_ctime = (uint32_t)now[0];
        r = ext4_write_inode(sb, ino, &victim);
        if (r < 0 && sb->journal) (void)ext4_journal_poison(sb);
        return r;
    }

    /* Keep last-link cleanup usable on ext2-like ext4 volumes without JBD2.
     * The same ordered orphan records are written through there, but without
     * a journal they cannot promise transaction-level crash atomicity. */
    if (victim.i_links_count != 1) {
        printf("[EXT4] invalid last-link count ino=%u links=%u\n",
               ino, victim.i_links_count);
        return -EOPNOTSUPP;
    }
    r = ext4_orphan_add_last_link(sb, ino);
    if (r < 0) {
        printf("[EXT4] orphan add failed ino=%u err=%d\n", ino, r);
        return r;
    }
    r = ext4_dir_remove(sb, di, dir_ino, ext4_inode_size(di), name);
    if (r < 0) {
        printf("[EXT4] unlink dirent remove failed ino=%u err=%d\n", ino, r);
        (void)ext4_journal_poison(sb);
        return r;
    }

    /* If a vnode is still alive (open fd, dcache, ...), retain its zero-link
     * inode on the on-disk orphan chain. Final close completes the same
     * journaled cleanup; a crash is recovered before the next mount is used.
     * C/R paired runs with identical source, workload and input reported the
     * same deferred-orphan count, so this backend does not attribute that
     * observation to a dcache implementation. */
    vnode_t *live = ext4_vnode_cache_remove(sb, ino);
    if (live) {
        ext4_vnode_priv_t *vp = (ext4_vnode_priv_t *)live->fs_data;
        printf("[EXT4] unlink orphan ino=%u refs=%d vfile_opens=%u\n", ino,
               vnode_ref_read(live), vp ?
               __atomic_load_n(&vp->open_count, __ATOMIC_ACQUIRE) : 0);
        if (vp) {
            /* Remember which chain link this vnode owns, so the deferred reap
             * can tell "still mine" from "already reaped" or "this inode
             * number now belongs to another file". */
            ext4_inode_t cur;
            if (ext4_read_inode(sb, ino, &cur) == 0)
                __atomic_store_n(&vp->orphan_next, cur.i_dtime,
                                 __ATOMIC_RELEASE);
            __atomic_store_n(&vp->unlinked, 1, __ATOMIC_RELEASE);
        }
        /* The final link is gone, but open descriptors must retain dirty data
         * until their last close.  With no open descriptor, discard unpinned
         * pages so deleted temporary files do not consume writeback bandwidth;
         * pinned pages (for example an mmap) remain attached. */
        if (!vp || __atomic_load_n(&vp->open_count, __ATOMIC_ACQUIRE) == 0)
            page_cache_discard_unlinked(live);
        if (deferred_put)
            *deferred_put = live;
        return 0;
    }

    r = ext4_orphan_reap(sb, ino);
    if (r < 0) {
        (void)ext4_journal_poison(sb);
        return r;
    }
    vfs_drop_time_meta_identity(mnt, ino);
    return 0;
}


int ext4_lookup_unlocked(vnode_t *dir, const char *name, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    if (strcmp(name, ".") == 0)  { *out = dir; vnode_get(dir); return 0; }
    if (strcmp(name, "..") == 0) {
        if (dir->parent) {
            *out = dir->parent;
            vnode_get(dir->parent);
            vnode_get(dir);          /* vnode_lookup_path will vnode_put(dir) */
            return 0;
        }
        *out = dir; vnode_get(dir); return 0;
    }

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;

    ext4_inode_t ci;
    if (ext4_read_inode(p->sb, child_ino, &ci) < 0) return -EIO;

    int type = VFS_FT_REGULAR;
    if (ft == EXT4_FT_DIR) type = VFS_FT_DIR;
    else if (ft == EXT4_FT_SYMLINK) type = VFS_FT_SYMLINK;
    else if (ft == EXT4_FT_FIFO) type = VFS_FT_FIFO;
    *out = ext4_make_vnode(p->sb, child_ino, ext4_inode_size(&ci), type, dir);
    if (!*out) return -ENOMEM;
    /* parent ref_count bumped in make_vnode */
    return 0;
}


int ext4_stat(vnode_t *vn, kstat_t *st) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    uint64_t sz = p->file_size;
    if (vn->size > sz)
        sz = vn->size;
    ext4_inode_t dinode;
    memset(&dinode, 0, sizeof(dinode));
    if (ext4_read_inode(p->sb, p->inode_num, &dinode) == 0) {
        uint64_t disk_size = ext4_inode_size(&dinode);
        if (disk_size > sz)
            sz = disk_size;
        p->file_size = sz;
        vn->size = sz;
    }
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->ino;
    st->st_size = sz;
    st->st_blksize = p->sb->block_size;
    st->st_blocks  = (sz + 511) / 512;
    st->st_mode = dinode.i_mode ? dinode.i_mode : vn->mode;
    uint32_t vtype_mode = S_IFREG;
    if (vn->type == VFS_FT_DIR)
        vtype_mode = S_IFDIR;
    else if (vn->type == VFS_FT_SYMLINK)
        vtype_mode = S_IFLNK;
    else if (vn->type == VFS_FT_FIFO)
        vtype_mode = S_IFIFO;
    if ((st->st_mode & S_IFMT) != vtype_mode)
        st->st_mode = (st->st_mode & ~S_IFMT) | vtype_mode;
    st->st_uid = dinode.i_uid;
    st->st_gid = dinode.i_gid;
    /* POSIX reports zero links for an open-but-unlinked inode. Preserve the
     * on-disk count so UFSD can distinguish a last unlink from a hard link. */
    st->st_nlink = dinode.i_links_count;
    /* Keep metadata fingerprints stable by exposing the on-disk inode times. */
    st->st_atime = dinode.i_atime;
    st->st_mtime = dinode.i_mtime;
    st->st_ctime = dinode.i_ctime;
    return 0;
}


/*
 * Finish a rmdir whose directory inode could not be freed at the time: the
 * caller has already removed the dirent and dropped the parent's link, so all
 * that is left is to truncate the directory, mark its inode deleted and free
 * it.  Shared by the immediate rmdir path and by the deferred path taken when
 * the child vnode was still cached.
 */
static int ext4_reclaim_removed_directory(ext4_sb_info_t *sb, uint32_t ino)
{
    ext4_inode_t cdi;
    if (ext4_read_inode(sb, ino, &cdi) < 0) return -EIO;
    ext4_block_truncate(sb, ino, &cdi);
    ext4_inode_mark_deleted(&cdi);
    if (ext4_write_inode(sb, ino, &cdi) < 0) return -EIO;
    return ext4_free_inode(sb, ino);
}


/* An ownership preflight has not modified metadata. A failed read leaves the
 * durable orphan entry for shutdown/recovery; it cannot poison other writers'
 * pending transaction. Mutation failures inside reap still abort as before. */
static int ext4_orphan_read_vnode_owner(ext4_sb_info_t *sb, uint32_t ino,
                                       ext4_inode_t *inode)
{
#ifdef CONFIG_EXT4_TEST_ORPHAN_READ_FAIL_ONCE
    static unsigned injected;
    if (!__atomic_exchange_n(&injected, 1, __ATOMIC_RELAXED)) {
        printf("[EXT4/TEST] injected orphan preflight read failure ino=%u\n", ino);
        return -EIO;
    }
#endif
    return ext4_read_inode(sb, ino, inode);
}

void ext4_release_vn(vnode_t *vn) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (p && p->unlinked) {
        /* Inode was unlinked while still referenced: reclaim its blocks
         * and the inode itself now that the last reference is gone. */
        ext4_sb_info_t *sb = p->sb;
        p->unlinked = 0;
        rw_mutex_write_lock(&sb->metadata_lock);
        ext4_inode_t cur;
        int read_ok = ext4_orphan_read_vnode_owner(sb, p->inode_num, &cur) == 0;
        /* Reaping by inode number alone is wrong once the entry is gone or the
         * number has been reused: the reap would fail noisily (and poison a
         * healthy journal) or free another file's inode. */
        int owned = read_ok && cur.i_links_count == 0 &&
                    cur.i_dtime == __atomic_load_n(&p->orphan_next,
                                                   __ATOMIC_ACQUIRE);
        int ret = !read_ok ? -EIO : (owned ? ext4_orphan_reap(sb, p->inode_num)
                                           : 0);
        if (ret < 0) {
            if (read_ok && sb->journal) (void)ext4_journal_poison(sb);
            if (read_ok)
                printf("[EXT4] deferred orphan cleanup failed ino=%u err=%d; "
                       "orphan retained for recovery\n", p->inode_num, ret);
            else {
                static unsigned reports;
                if (__atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 16)
                    printf("[EXT4] orphan preflight read deferred ino=%u err=%d; "
                           "unchanged orphan retained for shutdown/recovery\n", p->inode_num, ret);
            }
        } else {
            vfs_drop_time_meta_identity(vn->mnt, p->inode_num);
        }
        rw_mutex_write_unlock(&sb->metadata_lock);
    } else if (p && __atomic_load_n(&p->dir_removed, __ATOMIC_ACQUIRE)) {
        /* A directory removed while this vnode was cached.  No orphan entry
         * exists to recover it later, so the free happens here. */
        ext4_sb_info_t *sb = p->sb;
        __atomic_store_n(&p->dir_removed, 0, __ATOMIC_RELEASE);
        rw_mutex_write_lock(&sb->metadata_lock);
        int ret = ext4_reclaim_removed_directory(sb, p->inode_num);
        rw_mutex_write_unlock(&sb->metadata_lock);
        if (ret < 0)
            printf("[EXT4] deferred directory cleanup failed ino=%u err=%d; "
                   "inode left allocated\n", p->inode_num, ret);
        else
            vfs_drop_time_meta_identity(vn->mnt, p->inode_num);
    }
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    if (vn->parent && vn->parent != vn) vnode_put(vn->parent);
    kfree(vn);
}

/* Explicit last-close cleanup for the user-space backend. Keeping this as an
 * error-returning operation lets Rust retain a bounded pending slot and the
 * adapter's registry reference if metadata IO fails. The legacy orphan node
 * remains on disk until the Rust transaction succeeds, so restart recovery
 * is also available if the service dies before retry. */
int ext4_vnode_finalize_unlinked(vnode_t *vn)
{
    if (!vn || !vn->fs_data) return -EINVAL;
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (__atomic_load_n(&p->dir_removed, __ATOMIC_ACQUIRE)) {
        ext4_sb_info_t *dir_sb = p->sb;
        if (!dir_sb) return -EIO;
        rw_mutex_write_lock(&dir_sb->metadata_lock);
        int dir_ret = ext4_reclaim_removed_directory(dir_sb, p->inode_num);
        rw_mutex_write_unlock(&dir_sb->metadata_lock);
        if (dir_ret == 0) {
            __atomic_store_n(&p->dir_removed, 0, __ATOMIC_RELEASE);
            vfs_drop_time_meta_identity(vn->mnt, p->inode_num);
        }
        return dir_ret;
    }
    if (!__atomic_load_n(&p->unlinked, __ATOMIC_ACQUIRE)) return 0;
    ext4_sb_info_t *sb = p->sb;
    if (!sb) return -EIO;
    rw_mutex_write_lock(&sb->metadata_lock);
    if (!__atomic_load_n(&p->unlinked, __ATOMIC_ACQUIRE)) {
        rw_mutex_write_unlock(&sb->metadata_lock);
        return 0;
    }
    int ret;
    ext4_inode_t cur;
    int read_ok = ext4_orphan_read_vnode_owner(sb, p->inode_num, &cur) == 0;
    int owned = read_ok && cur.i_links_count == 0 &&
                cur.i_dtime == __atomic_load_n(&p->orphan_next,
                                               __ATOMIC_ACQUIRE);
    if (!read_ok) {
        ret = -EIO;
    } else if (!owned) {
        ret = 0;   /* entry already consumed, or the number was reused */
    } else {
        ret = ext4_orphan_reap(sb, p->inode_num);
    }
    if (ret < 0) {
        if (read_ok && sb->journal) (void)ext4_journal_poison(sb);
    } else {
        __atomic_store_n(&p->unlinked, 0, __ATOMIC_RELEASE);
        vfs_drop_time_meta_identity(vn->mnt, p->inode_num);
    }
    rw_mutex_write_unlock(&sb->metadata_lock);
    return ret;
}


int ext4_vn_create_unlocked(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    /* Check if already exists */
    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Initialize new inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    task_t *cur = proc_current();
    ni.i_mode = ((mode & S_IFMT) == S_IFIFO ? S_IFIFO : S_IFREG) | (mode & 07777);
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ni.i_links_count = 1;
    /* ext4 keeps a file's block map as an extent tree, not as the twelve
     * indirect block pointers of an ext2 inode.  Without the flag and an empty
     * tree header the writer falls back to i_block[] holding raw block
     * numbers, and e2fsck then reports the file's block map as unrecognised
     * (magic 0x50d9). */
    if ((ni.i_mode & S_IFMT) == S_IFREG) {
    ni.i_flags |= EXT4_EXTENTS_FL;
    {
        uint8_t *raw = (uint8_t *)&ni + offsetof(ext4_inode_t, i_block);
        ext4_extent_header_t hdr;
        hdr.eh_magic = EXT4_EXT_MAGIC;
        hdr.eh_entries = 0;
        hdr.eh_max = EXT4_EXT_ROOT_MAX;
        hdr.eh_depth = 0;
        hdr.eh_generation = 0;
        memcpy(raw, &hdr, sizeof(hdr));
    }
    }
    if (ext4_write_inode(p->sb, new_ino, &ni) < 0) {
        ext4_free_inode(p->sb, new_ino);
        return -EIO;
    }

    /* Add dir entry */
    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, p->inode_num, &dsz, name, new_ino, (ni.i_mode & S_IFMT) == S_IFIFO ? EXT4_FT_FIFO : EXT4_FT_REG_FILE);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        if (ext4_write_inode(p->sb, p->inode_num, &di) < 0)
            return -EIO;
        p->file_size = dsz;
    }

    if (out) {
        *out = ext4_make_vnode(p->sb, new_ino, 0, (ni.i_mode & S_IFMT) == S_IFIFO ? VFS_FT_FIFO : VFS_FT_REGULAR, dir);
        if (!*out) return -ENOMEM;
    }
    return 0;
}


int ext4_vn_mkdir_unlocked(vnode_t *dir, const char *name, int mode) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Allocate a block for the new directory */
    uint64_t blk = ext4_alloc_block(p->sb);
    if (!blk) { ext4_free_inode(p->sb, new_ino); return -ENOSPC; }

    /* Initialize new directory inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    task_t *cur = proc_current();
    ni.i_mode = S_IFDIR | (mode & 07777);
    if (di.i_mode & S_ISGID)
        ni.i_mode |= S_ISGID;
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ext4_inode_set_size(&ni, p->sb->block_size);
    ni.i_links_count = 2; /* . and .. */
    ni.i_flags |= EXT4_EXTENTS_FL;

    /* The group this inode lands in now owns one more directory; e2fsck
     * recounts them and reports the group whose descriptor disagrees. */
    ext4_account_directory(p->sb, new_ino, 1);

    /* Write extent for the one block */
    uint8_t *raw = (uint8_t *)&ni + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    hdr.eh_magic = EXT4_EXT_MAGIC; hdr.eh_entries = 1;
    hdr.eh_max = EXT4_EXT_ROOT_MAX; hdr.eh_depth = 0; hdr.eh_generation = 0;
    memcpy(raw, &hdr, sizeof(hdr));
    ext4_extent_t ext;
    ext.ee_block = 0; ext.ee_len = 1;
    ext.ee_start_hi = (uint16_t)(blk >> 32);
    ext.ee_start_lo = (uint32_t)(blk & 0xFFFFFFFF);
    memcpy(raw + sizeof(hdr), &ext, sizeof(ext));
    /* i_blocks has to describe the one block the new directory just claimed. */
    ext4_inode_sync_i_blocks(p->sb, &ni);

    if (ext4_write_inode(p->sb, new_ino, &ni) < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return -EIO;
    }

    /* Write "." and ".." entries in the new directory block */
    char *buf = (char *)kmalloc(p->sb->block_size);
    if (!buf) { ext4_free_block(p->sb, blk); ext4_free_inode(p->sb, new_ino); return -ENOMEM; }
    memset(buf, 0, p->sb->block_size);

    /* "." entry */
    ext4_dir_entry_t *dot = (ext4_dir_entry_t *)buf;
    dot->inode = new_ino;
    dot->name_len = 1;
    dot->file_type = EXT4_FT_DIR;
    dot->rec_len = 12;
    dot->name[0] = '.';

    /* ".." entry */
    ext4_dir_entry_t *dotdot = (ext4_dir_entry_t *)(buf + 12);
    dotdot->inode = p->inode_num;
    dotdot->name_len = 2;
    dotdot->file_type = EXT4_FT_DIR;
    /* ".." has to stop short of the tail: an entry that claimed the last
     * twelve bytes would hide the dirent tail from the walker, and e2fsck
     * then reports the block as having no checksum at all. */
    dotdot->rec_len = (uint16_t)(p->sb->block_size - 12 -
                                 ext4_dir_tail_reserve(p->sb));
    dotdot->name[0] = '.'; dotdot->name[1] = '.';

    int wr = ext4_dir_write_block(p->sb, new_ino, ni.i_generation, blk, buf);
    kfree(buf);
    if (wr < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return wr;
    }

    /* Add entry in parent directory */
    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, p->inode_num, &dsz, name, new_ino, EXT4_FT_DIR);
    if (r < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    /* The parent gains a subdirectory, and a directory's link count is the
     * number of its own "." plus one per subdirectory's "..".  e2fsck recounts
     * the tree in pass 4 and reports "Inode 2 ref count is 4, should be 10" when
     * the inode disagrees, so the parent is written back even when the entry
     * fitted in the block it already had. */
    di.i_links_count++;
    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        p->file_size = dsz;
    }
    if (ext4_write_inode(p->sb, p->inode_num, &di) < 0)
        return -EIO;
    return 0;
}


int ext4_vn_link_unlocked(vnode_t *dir, const char *name, vnode_t *target) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;
    if (!target || !target->fs_data) return -EINVAL;
    if (target->type == VFS_FT_DIR) return -EPERM;

    ext4_vnode_priv_t *tp = (ext4_vnode_priv_t *)target->fs_data;
    if (tp->sb != p->sb) return -EXDEV;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    /* Linux refuses to hard-link across different mounts; same sb is enough
     * here, but also refuse to re-link to the very same directory entry. */
    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0) {
        if (existing_ino == tp->inode_num) return -EEXIST;
        return -EEXIST;
    }
    if (lookup != -ENOENT)
        return lookup;

    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, tp->inode_num, &inode) < 0) return -EIO;

    inode.i_links_count++;
    uint64_t now[2];
    timekeeping_get_realtime(now);
    inode.i_ctime = (uint32_t)now[0];
    if (ext4_write_inode(p->sb, tp->inode_num, &inode) < 0) return -EIO;

    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, p->inode_num, &dsz, name, tp->inode_num, (target->mode & S_IFMT) == S_IFIFO ? EXT4_FT_FIFO : EXT4_FT_REG_FILE);
    if (r < 0) {
        /* Roll back the link count on failure. */
        inode.i_links_count--;
        ext4_write_inode(p->sb, tp->inode_num, &inode);
        return r;
    }
    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}


int ext4_vn_link(vnode_t *dir, const char *name, vnode_t *target) {
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_link_unlocked(dir, name, target);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_unlink_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft == EXT4_FT_DIR) return -EISDIR;

    return ext4_inode_remove(p->sb, dir->mnt, p->inode_num, &di, name,
                             child_ino, deferred_put);
}


int ext4_dir_empty(ext4_sb_info_t *sb, ext4_inode_t *di, uint64_t dsz) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (ext4_dir_entry_check(de, off, bs, NULL) < 0) {
                kfree(blk); return -EIO;
            }
            if (de->inode) {
                if (de->name_len == 1 && de->name[0] == '.') { off += de->rec_len; continue; }
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') { off += de->rec_len; continue; }
                kfree(blk); return -ENOTEMPTY;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    return 0;
}


int ext4_vn_rmdir_unlocked(vnode_t *dir, const char *name, vnode_t **deferred_put) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft != EXT4_FT_DIR) return -ENOTDIR;

    ext4_inode_t cdi;
    if (ext4_read_inode(p->sb, child_ino, &cdi) < 0) return -EIO;
    r = ext4_dir_empty(p->sb, &cdi, ext4_inode_size(&cdi));
    if (r < 0) return r;

    /* A directory's last two links are its own "." and its parent's "..",
     * both of which disappear with it, so it cannot go through the unlink path
     * -- that one only drops the first of the two and leaves the directory
     * allocated forever.  The parent loses one link, the child loses both plus
     * its blocks. */
    r = ext4_dir_remove(p->sb, &di, p->inode_num, ext4_inode_size(&di), name);
    if (r < 0) return r;

    if (di.i_links_count > 0) di.i_links_count--;
    if (ext4_write_inode(p->sb, p->inode_num, &di) < 0) return -EIO;
    ext4_account_directory(p->sb, child_ino, -1);

    vnode_t *live = ext4_vnode_cache_remove(p->sb, child_ino);
    if (live) {
        ext4_vnode_priv_t *vp = (ext4_vnode_priv_t *)live->fs_data;
        /* The directory's own two links went away with it, so unlike an
         * unlinked file there is no orphan entry a later mount could recover
         * from: record the pending free on the vnode and let
         * ext4_release_vn() finish it. */
        if (vp)
            __atomic_store_n(&vp->dir_removed, 1, __ATOMIC_RELEASE);
        if (!vp || __atomic_load_n(&vp->open_count, __ATOMIC_ACQUIRE) == 0)
            page_cache_discard_unlinked(live);
        if (deferred_put) *deferred_put = live;
        return 0;
    }

    return ext4_reclaim_removed_directory(p->sb, child_ino);
}


int ext4_vn_rename_unlocked(vnode_t *old_dir, const char *old_name,
                                   vnode_t *new_dir, const char *new_name,
                                   unsigned int flags, vnode_t **deferred_put,
                                   vnode_t **deferred_parent_put) {
    ext4_vnode_priv_t *op = (ext4_vnode_priv_t *)old_dir->fs_data;
    ext4_vnode_priv_t *np = (ext4_vnode_priv_t *)new_dir->fs_data;
    if (op->type != VFS_FT_DIR || np->type != VFS_FT_DIR) return -ENOTDIR;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;
    if (op->sb == np->sb && op->inode_num == np->inode_num &&
        strcmp(old_name, new_name) == 0)
        return 0;

    ext4_inode_t odi, ndi;
    if (ext4_read_inode(op->sb, op->inode_num, &odi) < 0) return -EIO;
    if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;

    /* Find source */
    uint32_t src_ino; uint8_t src_ft;
    int r = ext4_dir_find(op->sb, &odi, ext4_inode_size(&odi), old_name, &src_ino,
                          &src_ft);
    if (r < 0) return r;

    /* Find target */
    uint32_t tgt_ino = 0; uint8_t tgt_ft = 0;
    int tgt_lookup = ext4_dir_find(np->sb, &ndi, ext4_inode_size(&ndi),
                                   new_name, &tgt_ino, &tgt_ft);
    if (tgt_lookup != 0 && tgt_lookup != -ENOENT)
        return tgt_lookup;
    int tgt_exists = tgt_lookup == 0;

    if (flags & RENAME_NOREPLACE) {
        if (tgt_exists) return -EEXIST;
    }

    if (flags & RENAME_EXCHANGE) {
        if (!tgt_exists) return -ENOENT;
        if (old_dir == new_dir && strcmp(old_name, new_name) == 0) return 0;

        uint64_t odsz = ext4_inode_size(&odi);
        uint64_t ndsz = ext4_inode_size(&ndi);

        r = ext4_dir_update_entry(op->sb, &odi, op->inode_num, &odsz, old_name, tgt_ino, tgt_ft);
        if (r < 0) return r;
        r = ext4_dir_update_entry(np->sb, &ndi, np->inode_num, &ndsz, new_name, src_ino, src_ft);
        if (r < 0) {
            ext4_dir_update_entry(op->sb, &odi, op->inode_num, &odsz, old_name, src_ino, src_ft);
            return r;
        }
        if (odsz != ext4_inode_size(&odi)) {
            ext4_inode_set_size(&odi, odsz);
            ext4_write_inode(op->sb, op->inode_num, &odi);
            op->file_size = odsz;
        }
        if (ndsz != ext4_inode_size(&ndi)) {
            ext4_inode_set_size(&ndi, ndsz);
            ext4_write_inode(np->sb, np->inode_num, &ndi);
            np->file_size = ndsz;
        }
        return 0;
    }

    /* Check if target exists — if so, remove it */
    if (tgt_exists) {
        r = ext4_inode_remove(np->sb, new_dir->mnt, np->inode_num, &ndi,
                              new_name, tgt_ino, deferred_put);
        if (r < 0) return r;
        /* Re-read ndi after modification */
        if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;
    }

    /* Add new entry in target dir */
    uint64_t ndsz = ext4_inode_size(&ndi);
    r = ext4_dir_add(np->sb, &ndi, np->inode_num, &ndsz, new_name, src_ino, src_ft);
    if (r < 0) return r;
    if (ndsz != ext4_inode_size(&ndi)) {
        ext4_inode_set_size(&ndi, ndsz);
        ext4_write_inode(np->sb, np->inode_num, &ndi);
        np->file_size = ndsz;
    }

    if (ext4_read_inode(op->sb, op->inode_num, &odi) < 0)
        return -EIO;
    r = ext4_dir_remove(op->sb, &odi, op->inode_num, ext4_inode_size(&odi), old_name);
    if (r < 0) {
        /* Attempt rollback: remove the new entry */
        ext4_dir_remove(np->sb, &ndi, np->inode_num, ndsz, new_name);
        return r;
    }

    /* The moved inode keeps its single cached vnode; repoint its parent so
     * that ".." resolution follows the new location.  The old parent
     * reference is dropped by the caller after the lock is released. */
    vnode_t *moved = ext4_vnode_cache_lookup(op->sb, src_ino);
    if (moved && moved->parent && moved->parent != new_dir) {
        if (deferred_parent_put)
            *deferred_parent_put = moved->parent;
        vnode_get(new_dir);
        moved->parent = new_dir;
    }
    if (moved)
        vnode_put(moved);
    return 0;
}


int ext4_readlink(vnode_t *vn, char *buf, size_t sz) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (vn->type != VFS_FT_SYMLINK) return -EINVAL;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    size_t len = p->file_size;
    if (len > 60) len = 60; /* fast symlink limit */
    if (len >= sz) len = sz - 1;
    if (len > 0) {
        const char *target = (const char *)inode.i_block.i_data.i_block;
        memcpy(buf, target, len);
    }
    buf[len] = '\0';
    return (int)len;
}


int ext4_vn_symlink_unlocked(vnode_t *dir, const char *name,
                                    const char *target) {    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    int lookup = ext4_dir_find(p->sb, &di, ext4_inode_size(&di), name,
                               &existing_ino, NULL);
    if (lookup == 0)
        return -EEXIST;
    if (lookup != -ENOENT)
        return lookup;

    size_t tlen = strlen(target);
    if (tlen > 60) return -ENAMETOOLONG; /* fast symlink only */

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = S_IFLNK | 0777;
    task_t *cur = proc_current();
    ni.i_uid = cur ? (uint16_t)cur->cred.fsuid : 0;
    ni.i_gid = (di.i_mode & S_ISGID) ? di.i_gid : (cur ? (uint16_t)cur->cred.fsgid : 0);
    ni.i_links_count = 1;
    ext4_inode_set_size(&ni, tlen);
    memcpy(ni.i_block.i_data.i_block, target, tlen);
    ext4_write_inode(p->sb, new_ino, &ni);

    uint64_t dsz = ext4_inode_size(&di);
    int r = ext4_dir_add(p->sb, &di, p->inode_num, &dsz, name, new_ino, EXT4_FT_SYMLINK);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }
    if (dsz != ext4_inode_size(&di)) {
        ext4_inode_set_size(&di, dsz);
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}


int ext4_vn_truncate_unlocked(vnode_t *vn, size_t size) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (!p) return -EINVAL;
    if (vn->type == VFS_FT_DIR) return -EISDIR;

    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;

    uint64_t old_size = ext4_inode_size(&inode);
    int size_committed_by_txn = 0;
    if (size == 0) {
#ifdef CONFIG_EXT4_RUST_EXTENT_TXN
        int trunc_ret;
        if (inode.i_flags & EXT4_EXTENTS_FL) {
            trunc_ret = ext4_extent_partial_truncate_checked(
                p->sb, p->inode_num, &inode, 0, 0);
            size_committed_by_txn = trunc_ret >= 0;
        } else {
            trunc_ret = ext4_block_truncate_checked(
                p->sb, p->inode_num, &inode);
        }
#else
        int trunc_ret = ext4_block_truncate_checked(p->sb, p->inode_num, &inode);
#endif
        if (trunc_ret < 0)
            return trunc_ret;
    } else if (size < old_size) {
        /* Reclaim blocks beyond the new EOF instead of leaking them.  The
         * first logical block to drop is the one holding the new EOF. */
        uint32_t bs = p->sb->block_size;
        uint32_t lblk = (uint32_t)((size + bs - 1) / bs);
        if (lblk < (old_size + bs - 1) / bs) {
#ifdef CONFIG_EXT4_RUST_EXTENT_TXN
            if (inode.i_flags & EXT4_EXTENTS_FL) {
                /* Rust owns the reclaim decision and ordering; the C
                 * reference path below retains the historical behaviour for
                 * the diagnostic C-selector builds. */
                int trunc_ret = ext4_extent_partial_truncate_checked(
                    p->sb, p->inode_num, &inode, lblk, size);
                if (trunc_ret < 0)
                    return trunc_ret;
                size_committed_by_txn = 1;
            } else {
                int trunc_ret = ext4_block_truncate_at_checked(
                    p->sb, p->inode_num, &inode, lblk);
                if (trunc_ret < 0)
                    return trunc_ret;
            }
#else
            int trunc_ret = ext4_block_truncate_at_checked(
                p->sb, p->inode_num, &inode, lblk);
            if (trunc_ret < 0)
                return trunc_ret;
#endif
        }
    }

    if (!size_committed_by_txn) {
        ext4_inode_set_size(&inode, size);
        int write_ret = ext4_write_inode(p->sb, p->inode_num, &inode);
        if (write_ret < 0)
            return write_ret;
    }
    p->file_size = size;
    vn->size = size;
    return 0;
}


static int ext4_vn_chmod_unlocked(vnode_t *vn, int mode) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    inode.i_mode = (inode.i_mode & S_IFMT) | (mode & 07777);
    if (ext4_write_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    vn->mode = inode.i_mode;
    return 0;
}


static int ext4_vn_chown_unlocked(vnode_t *vn, int uid, int gid) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    if (uid != -1) inode.i_uid = (uint16_t)uid;
    if (gid != -1) inode.i_gid = (uint16_t)gid;
    if (uid != -1 || gid != -1) {
        inode.i_mode &= ~S_ISUID;
        if (inode.i_mode & S_IXGRP)
            inode.i_mode &= ~S_ISGID;
    }
    if (ext4_write_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;
    vn->uid = inode.i_uid;
    vn->gid = inode.i_gid;
    vn->mode = inode.i_mode;
    return 0;
}


int ext4_vn_chmod(vnode_t *vn, int mode)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int ret = ext4_vn_chmod_unlocked(vn, mode);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return ret;
}


int ext4_vn_chown(vnode_t *vn, int uid, int gid)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int ret = ext4_vn_chown_unlocked(vn, uid, gid);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return ret;
}


int ext4_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    /* Read mode: lookup is read-only through the block cache and the
     * independently locked vnode cache, so cold lookups on different
     * directories (or names) may run concurrently across CPUs.  It remains
     * mutually exclusive with every namespace mutation below. */
    rw_mutex_read_lock(&sb->metadata_lock);
    int r = ext4_lookup_unlocked(dir, name, out);
    rw_mutex_read_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_create(vnode_t *dir, const char *name, int mode,
                          vnode_t **out)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_create_unlocked(dir, name, mode, out);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_mkdir(vnode_t *dir, const char *name, int mode)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_mkdir_unlocked(dir, name, mode);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_unlink(vnode_t *dir, const char *name)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    vnode_t *deferred = NULL;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_unlink_unlocked(dir, name, &deferred);
    rw_mutex_write_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    return r;
}


int ext4_vn_rmdir(vnode_t *dir, const char *name)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    vnode_t *deferred = NULL;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_rmdir_unlocked(dir, name, &deferred);
    rw_mutex_write_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    return r;
}


int ext4_vn_rename(vnode_t *old_dir, const char *old_name,
                          vnode_t *new_dir, const char *new_name,
                          unsigned int flags)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)old_dir->fs_data)->sb;
    ext4_sb_info_t *new_sb = ((ext4_vnode_priv_t *)new_dir->fs_data)->sb;
    if (sb != new_sb)
        return -EXDEV;
    rw_mutex_write_lock(&sb->metadata_lock);
    vnode_t *deferred = NULL;
    vnode_t *deferred_parent = NULL;
    int r = ext4_vn_rename_unlocked(old_dir, old_name, new_dir, new_name,
                                    flags, &deferred, &deferred_parent);
    rw_mutex_write_unlock(&sb->metadata_lock);
    if (deferred)
        vnode_put(deferred);
    if (deferred_parent)
        vnode_put(deferred_parent);
    return r;
}


int ext4_vn_symlink(vnode_t *dir, const char *name,
                           const char *target)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)dir->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_symlink_unlocked(dir, name, target);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_truncate(vnode_t *vn, size_t size)
{
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = ext4_vn_truncate_unlocked(vn, size);
    rw_mutex_write_unlock(&sb->metadata_lock);
    return r;
}


int ext4_vn_statfs(vnode_t *vn, kstatfs_t *st)
{
    if (!vn || !vn->fs_data || !st)
        return -EINVAL;
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)vn->fs_data)->sb;
    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;

    mutex_lock(&sb->alloc_lock);
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        free_blocks += sb->group_descs[g].bg_free_blocks_count_lo;
        free_inodes += sb->group_descs[g].bg_free_inodes_count_lo;
    }
    mutex_unlock(&sb->alloc_lock);

    st->f_type = EXT4_DISK_MAGIC;
    st->f_bsize = sb->block_size;
    st->f_frsize = sb->block_size;
    st->f_blocks = sb->blocks_count;
    st->f_bfree = free_blocks;
    st->f_bavail = free_blocks > sb->reserved_blocks_count
                       ? free_blocks - sb->reserved_blocks_count : 0;
    st->f_files = sb->inodes_count;
    st->f_ffree = free_inodes;
    st->f_namelen = 255;
    return 0;
}
