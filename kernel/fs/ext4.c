#include "fs/ext4.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"

static int      ext4_read_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *out);
static int      ext4_write_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *inp);
static uint64_t ext4_block_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk);
static int      ext4_block_grow(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk, uint64_t phys);
static void     ext4_block_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode);
static vnode_t *ext4_make_vnode(ext4_sb_info_t *sb, uint32_t ino, uint32_t sz, int type, vnode_t *par);
static int      ext4_inode_remove(ext4_sb_info_t *sb, uint32_t dir_ino, ext4_inode_t *di, const char *name, uint32_t ino);

/* ================================================================
 * Vnode lifecycle
 *
 * Ext4 now follows the same short-lived vnode model as FAT32: every lookup
 * creates a fresh vnode, and the vnode is freed when its refcount drops to 0.
 *
 * The previous ext4-specific vnode cache tried to keep permanent references
 * by inode number, but in practice it amplified stale-pointer bugs into
 * deterministic panics on both architectures.  Keeping ext4 aligned with the
 * rest of the VFS is simpler and more robust.
 * ================================================================ */

static vnode_t *ext4_vnode_cache_lookup(ext4_sb_info_t *sb, uint32_t ino) {
    (void)sb;
    (void)ino;
    return NULL;
}

static void ext4_vnode_cache_insert(ext4_sb_info_t *sb, uint32_t ino, vnode_t *vn) {
    (void)sb;
    (void)ino;
    (void)vn;
}

static void ext4_vnode_cache_remove(ext4_sb_info_t *sb, uint32_t ino) {
    (void)sb;
    (void)ino;
}

/* ================================================================
 * Inode I/O
 * ================================================================ */

static int ext4_read_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *out) {
    if (ino < 1) return -EINVAL;
    uint32_t g = (ino - 1) / sb->inodes_per_group;
    uint32_t i = (ino - 1) % sb->inodes_per_group;
    if (g >= sb->groups_count) return -EINVAL;
    uint64_t it = (uint64_t)sb->group_descs[g].bg_inode_table_lo |
                  ((uint64_t)sb->group_descs[g].bg_inode_table_hi << 32);
    uint64_t off = it * sb->block_size + (uint64_t)i * sb->inode_size;
    memset(out, 0, sizeof(*out));
    int r = bcache_read_bytes(sb->bc, off, out, EXT4_INODE_SIZE_STATIC);
    return r < 0 ? -EIO : 0;
}

static int ext4_write_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *inp) {
    if (ino < 1) return -EINVAL;
    uint32_t g = (ino - 1) / sb->inodes_per_group;
    uint32_t i = (ino - 1) % sb->inodes_per_group;
    if (g >= sb->groups_count) return -EINVAL;
    uint64_t it = (uint64_t)sb->group_descs[g].bg_inode_table_lo |
                  ((uint64_t)sb->group_descs[g].bg_inode_table_hi << 32);
    uint64_t off = it * sb->block_size + (uint64_t)i * sb->inode_size;
    return bcache_write_bytes(sb->bc, off, inp, EXT4_INODE_SIZE_STATIC) < 0 ? -EIO : 0;
}

/* ================================================================
 * Bitmap / allocation
 * ================================================================ */

static void ext4_writeback_gd(ext4_sb_info_t *sb, uint32_t group) {
    uint64_t off = sb->block_group_desc_table_byte + (uint64_t)group * sb->desc_size;
    uint32_t n = sb->desc_size < sizeof(ext4_group_desc_t) ?
                 sb->desc_size : sizeof(ext4_group_desc_t);
    bcache_write_bytes(sb->bc, off, &sb->group_descs[group], n);
}

static int ext4_bitmap_alloc(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t max) {
    char *buf = (char *)kmalloc(sb->block_size);
    if (!buf) return -1;
    if (bcache_read_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size) < 0)
        { kfree(buf); return -1; }
    for (uint32_t i = 0; i < max; i++) {
        if (!(buf[i / 8] & (1 << (i % 8)))) {
            buf[i / 8] |= (1 << (i % 8));
            bcache_write_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size);
            kfree(buf); return (int)i;
        }
    }
    kfree(buf); return -1;
}

static void ext4_bitmap_free(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t bit) {
    char *buf = (char *)kmalloc(sb->block_size);
    if (!buf) return;
    if (bcache_read_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size) < 0)
        { kfree(buf); return; }
    buf[bit / 8] &= ~(1 << (bit % 8));
    bcache_write_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size);
    kfree(buf);
}

static uint64_t ext4_alloc_block(ext4_sb_info_t *sb) {
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        if (sb->group_descs[g].bg_free_blocks_count_lo == 0) continue;
        uint64_t bm = (uint64_t)sb->group_descs[g].bg_block_bitmap_lo |
                      ((uint64_t)sb->group_descs[g].bg_block_bitmap_hi << 32);
        int bit = ext4_bitmap_alloc(sb, bm, sb->blocks_per_group);
        if (bit < 0) continue;
        sb->group_descs[g].bg_free_blocks_count_lo--;
        ext4_writeback_gd(sb, g);
        uint64_t phys = (uint64_t)sb->first_data_block +
                        (uint64_t)g * sb->blocks_per_group + bit;
        char *z = (char *)kmalloc(sb->block_size);
        if (z) { memset(z, 0, sb->block_size);
                  bcache_write_bytes(sb->bc, phys * sb->block_size, z, sb->block_size);
                  kfree(z); }
        return phys;
    }
    return 0;
}

static void ext4_free_block(ext4_sb_info_t *sb, uint64_t phys) {
    uint32_t rel = (uint32_t)(phys - sb->first_data_block);
    uint32_t g = rel / sb->blocks_per_group;
    uint32_t bit = rel % sb->blocks_per_group;
    if (g >= sb->groups_count) return;
    uint64_t bm = (uint64_t)sb->group_descs[g].bg_block_bitmap_lo |
                  ((uint64_t)sb->group_descs[g].bg_block_bitmap_hi << 32);
    ext4_bitmap_free(sb, bm, bit);
    sb->group_descs[g].bg_free_blocks_count_lo++;
    ext4_writeback_gd(sb, g);
}

static uint32_t ext4_alloc_inode(ext4_sb_info_t *sb) {
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        if (sb->group_descs[g].bg_free_inodes_count_lo == 0) continue;
        uint64_t bm = (uint64_t)sb->group_descs[g].bg_inode_bitmap_lo |
                      ((uint64_t)sb->group_descs[g].bg_inode_bitmap_hi << 32);
        int bit = ext4_bitmap_alloc(sb, bm, sb->inodes_per_group);
        if (bit < 0) continue;
        sb->group_descs[g].bg_free_inodes_count_lo--;
        ext4_writeback_gd(sb, g);
        return g * sb->inodes_per_group + bit + 1;
    }
    return 0;
}

static void ext4_free_inode(ext4_sb_info_t *sb, uint32_t ino) {
    uint32_t g = (ino - 1) / sb->inodes_per_group;
    uint32_t bit = (ino - 1) % sb->inodes_per_group;
    if (g >= sb->groups_count) return;
    uint64_t bm = (uint64_t)sb->group_descs[g].bg_inode_bitmap_lo |
                  ((uint64_t)sb->group_descs[g].bg_inode_bitmap_hi << 32);
    ext4_bitmap_free(sb, bm, bit);
    sb->group_descs[g].bg_free_inodes_count_lo++;
    ext4_writeback_gd(sb, g);
}

/* ================================================================
 * Extent tree
 * ================================================================ */

static uint64_t ext4_extent_leaf_search(ext4_extent_t *ex, int cnt, uint32_t lblk) {
    for (int i = 0; i < cnt; i++) {
        uint32_t start = ex[i].ee_block;
        uint16_t len = ex[i].ee_len;
        if (len > 0x8000) len -= 0x8000;
        if (lblk >= start && lblk < start + len) {
            uint64_t p = (uint64_t)ex[i].ee_start_lo |
                         ((uint64_t)ex[i].ee_start_hi << 32);
            return p + (lblk - start);
        }
    }
    return 0;
}

#define EXT_PER_BLK(bs) (((bs) - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t))

static uint64_t ext4_extent_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) return 0;

    if (hdr.eh_depth == 0) {
        int n = hdr.eh_entries; if (n > 4) n = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), n * sizeof(ext4_extent_t));
        return ext4_extent_leaf_search(ext, n, lblk);
    }

    ext4_extent_idx_t idx0[4];
    int ni = hdr.eh_entries; if (ni > 4) ni = 4;
    memcpy(idx0, raw + sizeof(hdr), ni * sizeof(ext4_extent_idx_t));
    uint64_t next = 0;
    for (int i = 0; i < ni; i++)
        if (lblk >= idx0[i].ei_block)
            next = (uint64_t)idx0[i].ei_leaf_lo | ((uint64_t)idx0[i].ei_leaf_hi << 32);
    if (!next) return 0;

    for (int d = 1; d <= (int)hdr.eh_depth; d++) {
        char *b = (char *)kmalloc(sb->block_size);
        if (!b) return 0;
        if (bcache_read_bytes(sb->bc, next * sb->block_size, b, sb->block_size) < 0)
            { kfree(b); return 0; }
        ext4_extent_header_t eh; memcpy(&eh, b, sizeof(eh));
        if (eh.eh_magic != EXT4_EXT_MAGIC) { kfree(b); return 0; }
        if (eh.eh_depth == 0) {
            ext4_extent_t *ep = (ext4_extent_t *)(b + sizeof(eh));
            uint64_t r = ext4_extent_leaf_search(ep, eh.eh_entries, lblk);
            kfree(b); return r;
        }
        ext4_extent_idx_t *ip = (ext4_extent_idx_t *)(b + sizeof(eh));
        next = 0;
        for (int i = 0; i < eh.eh_entries; i++)
            if (lblk >= ip[i].ei_block)
                next = (uint64_t)ip[i].ei_leaf_lo | ((uint64_t)ip[i].ei_leaf_hi << 32);
        kfree(b); if (!next) return 0;
    }
    return 0;
}

static int ext4_extent_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                             uint32_t lblk, uint64_t pb) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    uint32_t epb = EXT_PER_BLK(sb->block_size);

    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) {
        hdr.eh_magic = EXT4_EXT_MAGIC; hdr.eh_entries = 1;
        hdr.eh_max = 4; hdr.eh_depth = 0; hdr.eh_generation = 0;
        memcpy(raw, &hdr, sizeof(hdr));
        ext4_extent_t e; e.ee_block = lblk; e.ee_len = 1;
        e.ee_start_hi = (uint16_t)(pb >> 32); e.ee_start_lo = (uint32_t)(pb & 0xFFFFFFFF);
        memcpy(raw + sizeof(hdr), &e, sizeof(e));
        inode->i_flags |= EXT4_EXTENTS_FL;
        return 0;
    }

    if (hdr.eh_depth == 0) {
        int n = hdr.eh_entries; if (n > 4) n = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), n * sizeof(ext4_extent_t));
        ext4_extent_t *le = &ext[n - 1];
        uint16_t ll = le->ee_len; if (ll > 0x8000) ll -= 0x8000;
        uint64_t lp = (uint64_t)le->ee_start_lo | ((uint64_t)le->ee_start_hi << 32);
        if (lblk == le->ee_block + ll && pb == lp + ll && ll < 0x8000) {
            le->ee_len++;
            memcpy(raw + sizeof(hdr), ext, n * sizeof(ext4_extent_t));
            return 0;
        }
        if (n < (int)hdr.eh_max) {
            ext[n].ee_block = lblk; ext[n].ee_len = 1;
            ext[n].ee_start_hi = (uint16_t)(pb >> 32);
            ext[n].ee_start_lo = (uint32_t)(pb & 0xFFFFFFFF);
            hdr.eh_entries = n + 1;
            memcpy(raw, &hdr, sizeof(hdr));
            memcpy(raw + sizeof(hdr), ext, (n + 1) * sizeof(ext4_extent_t));
            return 0;
        }
        uint64_t l1 = ext4_alloc_block(sb); if (!l1) return -ENOSPC;
        uint64_t l2 = ext4_alloc_block(sb);
        if (!l2) { ext4_free_block(sb, l1); return -ENOSPC; }
        char *b1 = (char *)kmalloc(sb->block_size);
        char *b2 = (char *)kmalloc(sb->block_size);
        if (!b1 || !b2) {
            kfree(b1); kfree(b2);
            ext4_free_block(sb, l1); ext4_free_block(sb, l2);
            return -ENOMEM;
        }
        memset(b1, 0, sb->block_size);
        ext4_extent_header_t lh1; lh1.eh_magic = EXT4_EXT_MAGIC;
        lh1.eh_entries = n; lh1.eh_max = epb; lh1.eh_depth = 0; lh1.eh_generation = 0;
        memcpy(b1, &lh1, sizeof(lh1));
        memcpy(b1 + sizeof(lh1), ext, n * sizeof(ext4_extent_t));
        bcache_write_bytes(sb->bc, l1 * sb->block_size, b1, sb->block_size);

        memset(b2, 0, sb->block_size);
        ext4_extent_header_t lh2; lh2.eh_magic = EXT4_EXT_MAGIC;
        lh2.eh_entries = 1; lh2.eh_max = epb; lh2.eh_depth = 0; lh2.eh_generation = 0;
        ext4_extent_t ne; ne.ee_block = lblk; ne.ee_len = 1;
        ne.ee_start_hi = (uint16_t)(pb >> 32); ne.ee_start_lo = (uint32_t)(pb & 0xFFFFFFFF);
        memcpy(b2, &lh2, sizeof(lh2));
        memcpy(b2 + sizeof(lh2), &ne, sizeof(ne));
        bcache_write_bytes(sb->bc, l2 * sb->block_size, b2, sb->block_size);
        kfree(b1); kfree(b2);

        hdr.eh_depth = 1; hdr.eh_entries = 2; hdr.eh_max = 4;
        memcpy(raw, &hdr, sizeof(hdr));
        ext4_extent_idx_t idx[2];
        idx[0].ei_block = ext[0].ee_block;
        idx[0].ei_leaf_lo = (uint32_t)(l1 & 0xFFFFFFFF);
        idx[0].ei_leaf_hi = (uint16_t)(l1 >> 32); idx[0].ei_unused = 0;
        idx[1].ei_block = lblk;
        idx[1].ei_leaf_lo = (uint32_t)(l2 & 0xFFFFFFFF);
        idx[1].ei_leaf_hi = (uint16_t)(l2 >> 32); idx[1].ei_unused = 0;
        memcpy(raw + sizeof(hdr), idx, 2 * sizeof(ext4_extent_idx_t));
        return 0;
    }

    ext4_extent_idx_t idx0[4];
    int ni = hdr.eh_entries; if (ni > 4) ni = 4;
    memcpy(idx0, raw + sizeof(hdr), ni * sizeof(ext4_extent_idx_t));
    uint64_t lb = 0;
    for (int i = ni - 1; i >= 0; i--) {
        lb = (uint64_t)idx0[i].ei_leaf_lo | ((uint64_t)idx0[i].ei_leaf_hi << 32);
        break;
    }
    if (hdr.eh_depth > 1) return -ENOSPC;

    char *leaf = (char *)kmalloc(sb->block_size);
    if (!leaf) return -ENOMEM;
    if (bcache_read_bytes(sb->bc, lb * sb->block_size, leaf, sb->block_size) < 0)
        { kfree(leaf); return -EIO; }
    ext4_extent_header_t lh; memcpy(&lh, leaf, sizeof(lh));

    if (lh.eh_entries > 0) {
        ext4_extent_t *ep = (ext4_extent_t *)(leaf + sizeof(lh));
        ext4_extent_t *le = &ep[lh.eh_entries - 1];
        uint16_t ll = le->ee_len; if (ll > 0x8000) ll -= 0x8000;
        uint64_t lp = (uint64_t)le->ee_start_lo | ((uint64_t)le->ee_start_hi << 32);
        if (lblk == le->ee_block + ll && pb == lp + ll && ll < 0x8000) {
            le->ee_len++;
            bcache_write_bytes(sb->bc, lb * sb->block_size, leaf, sb->block_size);
            kfree(leaf); return 0;
        }
    }
    if (lh.eh_entries < epb) {
        ext4_extent_t *ep = (ext4_extent_t *)(leaf + sizeof(lh));
        ep[lh.eh_entries].ee_block = lblk; ep[lh.eh_entries].ee_len = 1;
        ep[lh.eh_entries].ee_start_hi = (uint16_t)(pb >> 32);
        ep[lh.eh_entries].ee_start_lo = (uint32_t)(pb & 0xFFFFFFFF);
        lh.eh_entries++;
        memcpy(leaf, &lh, sizeof(lh));
        bcache_write_bytes(sb->bc, lb * sb->block_size, leaf, sb->block_size);
        kfree(leaf); return 0;
    }
    kfree(leaf);

    uint64_t nl = ext4_alloc_block(sb); if (!nl) return -ENOSPC;
    char *nb = (char *)kmalloc(sb->block_size);
    if (!nb) { ext4_free_block(sb, nl); return -ENOMEM; }
    memset(nb, 0, sb->block_size);
    ext4_extent_header_t nlh; nlh.eh_magic = EXT4_EXT_MAGIC;
    nlh.eh_entries = 1; nlh.eh_max = epb; nlh.eh_depth = 0; nlh.eh_generation = 0;
    ext4_extent_t newe; newe.ee_block = lblk; newe.ee_len = 1;
    newe.ee_start_hi = (uint16_t)(pb >> 32); newe.ee_start_lo = (uint32_t)(pb & 0xFFFFFFFF);
    memcpy(nb, &nlh, sizeof(nlh));
    memcpy(nb + sizeof(nlh), &newe, sizeof(newe));
    bcache_write_bytes(sb->bc, nl * sb->block_size, nb, sb->block_size);
    kfree(nb);

    if (ni < 4) {
        idx0[ni].ei_block = lblk;
        idx0[ni].ei_leaf_lo = (uint32_t)(nl & 0xFFFFFFFF);
        idx0[ni].ei_leaf_hi = (uint16_t)(nl >> 32); idx0[ni].ei_unused = 0;
        ni++; hdr.eh_entries = ni;
        memcpy(raw, &hdr, sizeof(hdr));
        memcpy(raw + sizeof(hdr), idx0, ni * sizeof(ext4_extent_idx_t));
        return 0;
    }
    ext4_free_block(sb, nl);
    return -ENOSPC;
}

static void ext4_extent_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr; memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) return;

    if (hdr.eh_depth == 0) {
        int n = hdr.eh_entries; if (n > 4) n = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), n * sizeof(ext4_extent_t));
        for (int i = 0; i < n; i++) {
            uint16_t len = ext[i].ee_len; if (len > 0x8000) len -= 0x8000;
            uint64_t s = (uint64_t)ext[i].ee_start_lo | ((uint64_t)ext[i].ee_start_hi << 32);
            for (uint16_t j = 0; j < len; j++) ext4_free_block(sb, s + j);
        }
    } else {
        ext4_extent_idx_t idx[4];
        int ni = hdr.eh_entries; if (ni > 4) ni = 4;
        memcpy(idx, raw + sizeof(hdr), ni * sizeof(ext4_extent_idx_t));
        for (int i = 0; i < ni; i++) {
            uint64_t lb = (uint64_t)idx[i].ei_leaf_lo | ((uint64_t)idx[i].ei_leaf_hi << 32);
            char *b = (char *)kmalloc(sb->block_size);
            if (!b) continue;
            if (bcache_read_bytes(sb->bc, lb * sb->block_size, b, sb->block_size) < 0)
                { kfree(b); continue; }
            ext4_extent_header_t lh; memcpy(&lh, b, sizeof(lh));
            if (lh.eh_magic == EXT4_EXT_MAGIC) {
                ext4_extent_t *ep = (ext4_extent_t *)(b + sizeof(lh));
                for (int j = 0; j < lh.eh_entries; j++) {
                    uint16_t len = ep[j].ee_len; if (len > 0x8000) len -= 0x8000;
                    uint64_t s = (uint64_t)ep[j].ee_start_lo | ((uint64_t)ep[j].ee_start_hi << 32);
                    for (uint16_t k = 0; k < len; k++) ext4_free_block(sb, s + k);
                }
            }
            kfree(b); ext4_free_block(sb, lb);
        }
    }
    ext4_extent_header_t rst; rst.eh_magic = EXT4_EXT_MAGIC;
    rst.eh_entries = 0; rst.eh_max = 4; rst.eh_depth = 0; rst.eh_generation = 0;
    memcpy(raw, &rst, sizeof(rst));
}

/* ================================================================
 * Indirect block mapping
 * ================================================================ */

static uint64_t ext4_indirect_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
    uint32_t b[15]; memcpy(b, inode->i_block.i_data.i_block, sizeof(b));
    uint32_t apb = sb->addr_per_block;
    if (lblk < 12) return (uint64_t)b[lblk];
    lblk -= 12;
    if (lblk < apb) {
        if (!b[12]) return 0;
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
        if (!ind) return 0;
        bcache_read_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
        uint32_t r = ind[lblk]; kfree(ind); return (uint64_t)r;
    }
    lblk -= apb;
    if (lblk < apb * apb && b[13]) {
        uint32_t *di = (uint32_t *)kmalloc(sb->block_size);
        if (!di) return 0;
        bcache_read_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
        uint32_t ii = lblk / apb, ib = di[ii]; kfree(di);
        if (!ib) return 0;
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
        if (!ind) return 0;
        bcache_read_bytes(sb->bc, (uint64_t)ib * sb->block_size, ind, sb->block_size);
        uint32_t r = ind[lblk % apb]; kfree(ind); return (uint64_t)r;
    }
    return 0;
}

static int ext4_indirect_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                               uint32_t lblk, uint64_t phys) {
    uint32_t b[15]; memcpy(b, inode->i_block.i_data.i_block, sizeof(b));
    uint32_t apb = sb->addr_per_block;
    if (lblk < 12) {
        b[lblk] = (uint32_t)phys;
    } else if ((lblk - 12) < apb) {
        if (!b[12]) { uint64_t nb = ext4_alloc_block(sb); if (!nb) return -ENOSPC; b[12] = (uint32_t)nb; }
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size); if (!ind) return -ENOMEM;
        bcache_read_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
        ind[lblk - 12] = (uint32_t)phys;
        bcache_write_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
        kfree(ind);
    } else if ((lblk - 12 - apb) < apb * apb) {
        uint32_t li = lblk - 12 - apb;
        if (!b[13]) { uint64_t nb = ext4_alloc_block(sb); if (!nb) return -ENOSPC; b[13] = (uint32_t)nb; }
        uint32_t *di = (uint32_t *)kmalloc(sb->block_size); if (!di) return -ENOMEM;
        bcache_read_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
        uint32_t ii = li / apb;
        if (!di[ii]) {
            uint64_t nb = ext4_alloc_block(sb);
            if (!nb) { kfree(di); return -ENOSPC; }
            di[ii] = (uint32_t)nb;
            bcache_write_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
        }
        uint32_t ib = di[ii]; kfree(di);
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size); if (!ind) return -ENOMEM;
        bcache_read_bytes(sb->bc, (uint64_t)ib * sb->block_size, ind, sb->block_size);
        ind[li % apb] = (uint32_t)phys;
        bcache_write_bytes(sb->bc, (uint64_t)ib * sb->block_size, ind, sb->block_size);
        kfree(ind);
    } else return -ENOSPC;
    memcpy(inode->i_block.i_data.i_block, b, sizeof(b));
    return 0;
}

static void ext4_indirect_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
    uint32_t b[15]; memcpy(b, inode->i_block.i_data.i_block, sizeof(b));
    uint32_t apb = sb->addr_per_block;
    for (int i = 0; i < 12; i++) if (b[i]) ext4_free_block(sb, b[i]);
    if (b[12]) {
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
        if (ind) { bcache_read_bytes(sb->bc, (uint64_t)b[12]*sb->block_size,ind,sb->block_size);
                    for (uint32_t i=0;i<apb;i++) if(ind[i]) ext4_free_block(sb,ind[i]);
                    kfree(ind); }
        ext4_free_block(sb, b[12]);
    }
    if (b[13]) {
        uint32_t *di = (uint32_t *)kmalloc(sb->block_size);
        if (di) { bcache_read_bytes(sb->bc, (uint64_t)b[13]*sb->block_size,di,sb->block_size);
                   for (uint32_t i=0;i<apb;i++) if(di[i]) {
                       uint32_t *ind=(uint32_t *)kmalloc(sb->block_size);
                       if(ind){bcache_read_bytes(sb->bc, (uint64_t)di[i]*sb->block_size,ind,sb->block_size);
                               for(uint32_t j=0;j<apb;j++) if(ind[j]) ext4_free_block(sb,ind[j]);
                               kfree(ind);}
                       ext4_free_block(sb,di[i]);
                   } kfree(di); }
        ext4_free_block(sb, b[13]);
    }
    memset(inode->i_block.i_data.i_block, 0, 60);
}

/* ---- generic dispatch ---- */

static uint64_t ext4_block_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext4_extent_map(sb, inode, lblk);
    return ext4_indirect_map(sb, inode, lblk);
}
static int ext4_block_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                            uint32_t lblk, uint64_t phys) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext4_extent_grow(sb, inode, lblk, phys);
    return ext4_indirect_grow(sb, inode, lblk, phys);
}
static void ext4_block_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
    if (inode->i_flags & EXT4_EXTENTS_FL) ext4_extent_truncate(sb, inode);
    else ext4_indirect_truncate(sb, inode);
}

/* ================================================================
 * Directory helpers
 * ================================================================ */

static int ext4_dir_find(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dsz,
                          const char *name, uint32_t *out_ino, uint8_t *out_ft) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (out_ino) *out_ino = de->inode;
                if (out_ft) *out_ft = de->file_type;
                kfree(blk); return 0;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    return -ENOENT;
}

static int ext4_dir_add(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t *dsz,
                         const char *name, uint32_t ino, uint8_t ft) {
    uint32_t bs = sb->block_size;
    size_t nl = strlen(name);
    uint16_t need = (uint16_t)((8 + nl + 3) & ~3);
    uint32_t nb = (*dsz + bs - 1) / bs;

    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (de->rec_len == 0) break;
            uint16_t actual = (uint16_t)((8 + de->name_len + 3) & ~3);
            if (de->inode == 0 && de->rec_len >= need) {
                de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
                memcpy(de->name, name, nl);
                if (de->rec_len > need) {
                    uint16_t left = de->rec_len - need; de->rec_len = need;
                    ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + need);
                    nx->inode = 0; nx->rec_len = left; nx->name_len = 0; nx->file_type = 0;
                }
                bcache_write_bytes(sb->bc, p * bs, blk, bs); kfree(blk); return 0;
            }
            if (de->rec_len - actual >= need) {
                uint16_t old = de->rec_len; de->rec_len = actual;
                ext4_dir_entry_t *nx = (ext4_dir_entry_t *)(blk + off + actual);
                nx->inode = ino; nx->rec_len = old - actual;
                nx->name_len = (uint8_t)nl; nx->file_type = ft;
                memcpy(nx->name, name, nl);
                bcache_write_bytes(sb->bc, p * bs, blk, bs); kfree(blk); return 0;
            }
            off += de->rec_len;
        }
        kfree(blk);
    }
    uint64_t nb_blk = ext4_alloc_block(sb); if (!nb_blk) return -ENOSPC;
    int gr = ext4_block_grow(sb, di, nb, nb_blk);
    if (gr < 0) { ext4_free_block(sb, nb_blk); return gr; }
    char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
    memset(blk, 0, bs);
    ext4_dir_entry_t *de = (ext4_dir_entry_t *)blk;
    de->inode = ino; de->name_len = (uint8_t)nl; de->file_type = ft;
    memcpy(de->name, name, nl);
    if (need < bs) {
        de->rec_len = need;
        ext4_dir_entry_t *tail = (ext4_dir_entry_t *)(blk + need);
        tail->inode = 0; tail->rec_len = (uint16_t)(bs - need); tail->name_len = 0;
    } else de->rec_len = (uint16_t)bs;
    bcache_write_bytes(sb->bc, nb_blk * bs, blk, bs); kfree(blk);
    *dsz += bs;
    return 0;
}

static int ext4_dir_remove(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dsz,
                            const char *name) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    size_t nl = strlen(name);
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0, prev = 0; int hp = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (de->rec_len == 0) break;
            if (de->inode && de->name_len == nl && memcmp(de->name, name, nl) == 0) {
                if (hp) ((ext4_dir_entry_t *)(blk + prev))->rec_len += de->rec_len;
                else de->inode = 0;
                bcache_write_bytes(sb->bc, p * bs, blk, bs); kfree(blk); return 0;
            }
            prev = off; hp = 1; off += de->rec_len;
        }
        kfree(blk);
    }
    return -ENOENT;
}

/* ================================================================
 * Shared inode removal helper (used by unlink and rename)
 * ================================================================ */

static int ext4_inode_remove(ext4_sb_info_t *sb, uint32_t dir_ino __attribute__((unused)),
                              ext4_inode_t *di, const char *name, uint32_t ino) {
    int r = ext4_dir_remove(sb, di, di->i_size_lo, name);
    if (r < 0) return r;

    ext4_inode_t victim;
    r = ext4_read_inode(sb, ino, &victim);
    if (r < 0) return r;

    ext4_block_truncate(sb, &victim);
    memset(&victim, 0, sizeof(victim));
    victim.i_dtime = 1;
    ext4_write_inode(sb, ino, &victim);
    ext4_free_inode(sb, ino);
    ext4_vnode_cache_remove(sb, ino);
    return 0;
}

/* ================================================================
 * VNode operations
 * ================================================================ */

static int ext4_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    if (strcmp(name, ".") == 0)  { *out = dir; dir->ref_count++; return 0; }
    if (strcmp(name, "..") == 0) {
        if (dir->parent) {
            *out = dir->parent;
            dir->parent->ref_count++;
            dir->ref_count++;          /* vnode_lookup_path will vnode_put(dir) */
            return 0;
        }
        *out = dir; dir->ref_count++; return 0;
    }

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, p->file_size, name, &child_ino, &ft);
    if (r < 0) return r;

    ext4_inode_t ci;
    if (ext4_read_inode(p->sb, child_ino, &ci) < 0) return -EIO;

    int type = VFS_FT_REGULAR;
    if (ft == EXT4_FT_DIR) type = VFS_FT_DIR;
    else if (ft == EXT4_FT_SYMLINK) type = VFS_FT_SYMLINK;
    *out = ext4_make_vnode(p->sb, child_ino, ci.i_size_lo, type, dir);
    if (!*out) return -ENOMEM;
    /* parent ref_count bumped in make_vnode */
    return 0;
}

static int ext4_stat(vnode_t *vn, kstat_t *st) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    uint64_t sz = p->file_size;
    ext4_inode_t dinode;
    if (ext4_read_inode(p->sb, p->inode_num, &dinode) == 0) {
        sz = dinode.i_size_lo;
        p->file_size = sz;
        vn->size = sz;
    }
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->ino;
    st->st_size = sz;
    st->st_blksize = p->sb->block_size;
    st->st_blocks  = (sz + 511) / 512;
    if (vn->type == VFS_FT_DIR) st->st_mode = S_IFDIR | 0755;
    else if (vn->type == VFS_FT_SYMLINK) st->st_mode = S_IFLNK | 0777;
    else st->st_mode = S_IFREG | 0755;
    st->st_nlink = 1;
    return 0;
}

static void ext4_release_vn(vnode_t *vn) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (p) ext4_vnode_cache_remove(p->sb, p->inode_num);
    if (vn->fs_data) { kfree(vn->fs_data); vn->fs_data = NULL; }
    vnode_put(vn->parent);
    kfree(vn);
}

static int ext4_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    (void)mode;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    /* Check if already exists */
    uint32_t existing_ino;
    if (ext4_dir_find(p->sb, &di, p->file_size, name, &existing_ino, NULL) == 0)
        return -EEXIST;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Initialize new inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = S_IFREG | 0755;
    ni.i_links_count = 1;
    ext4_write_inode(p->sb, new_ino, &ni);

    /* Add dir entry */
    uint32_t dsz = di.i_size_lo;
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_REG_FILE);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    if (dsz != di.i_size_lo) {
        di.i_size_lo = dsz;
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }

    if (out) {
        *out = ext4_make_vnode(p->sb, new_ino, 0, VFS_FT_REGULAR, dir);
        if (!*out) return -ENOMEM;
    }
    return 0;
}

static int ext4_vn_mkdir(vnode_t *dir, const char *name, int mode) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    (void)mode;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    if (ext4_dir_find(p->sb, &di, p->file_size, name, &existing_ino, NULL) == 0)
        return -EEXIST;

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    /* Allocate a block for the new directory */
    uint64_t blk = ext4_alloc_block(p->sb);
    if (!blk) { ext4_free_inode(p->sb, new_ino); return -ENOSPC; }

    /* Initialize new directory inode */
    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = S_IFDIR | 0755;
    ni.i_size_lo = p->sb->block_size;
    ni.i_links_count = 2; /* . and .. */
    ni.i_flags |= EXT4_EXTENTS_FL;

    /* Write extent for the one block */
    uint8_t *raw = (uint8_t *)&ni + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr;
    hdr.eh_magic = EXT4_EXT_MAGIC; hdr.eh_entries = 1;
    hdr.eh_max = 4; hdr.eh_depth = 0; hdr.eh_generation = 0;
    memcpy(raw, &hdr, sizeof(hdr));
    ext4_extent_t ext;
    ext.ee_block = 0; ext.ee_len = 1;
    ext.ee_start_hi = (uint16_t)(blk >> 32);
    ext.ee_start_lo = (uint32_t)(blk & 0xFFFFFFFF);
    memcpy(raw + sizeof(hdr), &ext, sizeof(ext));

    ext4_write_inode(p->sb, new_ino, &ni);

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
    dotdot->rec_len = (uint16_t)(p->sb->block_size - 12);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';

    bcache_write_bytes(p->sb->bc, blk * p->sb->block_size, buf, p->sb->block_size);
    kfree(buf);

    /* Add entry in parent directory */
    uint32_t dsz = di.i_size_lo;
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_DIR);
    if (r < 0) {
        ext4_free_block(p->sb, blk);
        ext4_free_inode(p->sb, new_ino);
        return r;
    }

    if (dsz != di.i_size_lo) {
        di.i_size_lo = dsz;
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}

static int ext4_vn_unlink(vnode_t *dir, const char *name) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, p->file_size, name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft == EXT4_FT_DIR) return -EISDIR;

    return ext4_inode_remove(p->sb, p->inode_num, &di, name, child_ino);
}

static int ext4_dir_empty(ext4_sb_info_t *sb, ext4_inode_t *di, uint32_t dsz) {
    uint32_t bs = sb->block_size, nb = (dsz + bs - 1) / bs;
    for (uint32_t b = 0; b < nb; b++) {
        uint64_t p = ext4_block_map(sb, di, b); if (!p) continue;
        char *blk = (char *)kmalloc(bs); if (!blk) return -ENOMEM;
        if (bcache_read_bytes(sb->bc, p * bs, blk, bs) < 0) { kfree(blk); return -EIO; }
        uint32_t off = 0;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (de->rec_len == 0) break;
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

static int ext4_vn_rmdir(vnode_t *dir, const char *name) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t child_ino; uint8_t ft;
    int r = ext4_dir_find(p->sb, &di, p->file_size, name, &child_ino, &ft);
    if (r < 0) return r;
    if (ft != EXT4_FT_DIR) return -ENOTDIR;

    ext4_inode_t cdi;
    if (ext4_read_inode(p->sb, child_ino, &cdi) < 0) return -EIO;
    r = ext4_dir_empty(p->sb, &cdi, cdi.i_size_lo);
    if (r < 0) return r;

    return ext4_inode_remove(p->sb, p->inode_num, &di, name, child_ino);
}

static int ext4_vn_rename(vnode_t *old_dir, const char *old_name,
                            vnode_t *new_dir, const char *new_name) {
    ext4_vnode_priv_t *op = (ext4_vnode_priv_t *)old_dir->fs_data;
    ext4_vnode_priv_t *np = (ext4_vnode_priv_t *)new_dir->fs_data;
    if (op->type != VFS_FT_DIR || np->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t odi, ndi;
    if (ext4_read_inode(op->sb, op->inode_num, &odi) < 0) return -EIO;
    if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;

    /* Find source */
    uint32_t src_ino; uint8_t src_ft;
    int r = ext4_dir_find(op->sb, &odi, op->file_size, old_name, &src_ino, &src_ft);
    if (r < 0) return r;

    /* Check if target exists — if so, remove it */
    uint32_t tgt_ino;
    if (ext4_dir_find(np->sb, &ndi, np->file_size, new_name, &tgt_ino, NULL) == 0) {
        r = ext4_inode_remove(np->sb, np->inode_num, &ndi, new_name, tgt_ino);
        if (r < 0) return r;
        /* Re-read ndi after modification */
        if (ext4_read_inode(np->sb, np->inode_num, &ndi) < 0) return -EIO;
    }

    /* Add new entry in target dir */
    uint32_t ndsz = ndi.i_size_lo;
    r = ext4_dir_add(np->sb, &ndi, &ndsz, new_name, src_ino, src_ft);
    if (r < 0) return r;
    if (ndsz != ndi.i_size_lo) {
        ndi.i_size_lo = ndsz;
        ext4_write_inode(np->sb, np->inode_num, &ndi);
        np->file_size = ndsz;
    }

    r = ext4_dir_remove(op->sb, &odi, op->file_size, old_name);
    if (r < 0) {
        /* Attempt rollback: remove the new entry */
        ext4_dir_remove(np->sb, &ndi, ndsz, new_name);
        return r;
    }
    return 0;
}

static int ext4_readlink(vnode_t *vn, char *buf, size_t sz) {
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

static int ext4_vn_symlink(vnode_t *dir, const char *name, const char *target) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)dir->fs_data;
    if (p->type != VFS_FT_DIR) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(p->sb, p->inode_num, &di) < 0) return -EIO;

    uint32_t existing_ino;
    if (ext4_dir_find(p->sb, &di, p->file_size, name, &existing_ino, NULL) == 0)
        return -EEXIST;

    size_t tlen = strlen(target);
    if (tlen > 60) return -ENAMETOOLONG; /* fast symlink only */

    uint32_t new_ino = ext4_alloc_inode(p->sb);
    if (!new_ino) return -ENOSPC;

    ext4_inode_t ni;
    memset(&ni, 0, sizeof(ni));
    ni.i_mode = S_IFLNK | 0777;
    ni.i_links_count = 1;
    ni.i_size_lo = (uint32_t)tlen;
    memcpy(ni.i_block.i_data.i_block, target, tlen);
    ext4_write_inode(p->sb, new_ino, &ni);

    uint32_t dsz = di.i_size_lo;
    int r = ext4_dir_add(p->sb, &di, &dsz, name, new_ino, EXT4_FT_SYMLINK);
    if (r < 0) {
        ext4_free_inode(p->sb, new_ino);
        return r;
    }
    if (dsz != di.i_size_lo) {
        di.i_size_lo = dsz;
        ext4_write_inode(p->sb, p->inode_num, &di);
        p->file_size = dsz;
    }
    return 0;
}

static int ext4_vn_truncate(vnode_t *vn, size_t size) {
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (size != 0) return -EINVAL; /* only support truncation to 0 */

    ext4_inode_t inode;
    if (ext4_read_inode(p->sb, p->inode_num, &inode) < 0) return -EIO;

    ext4_block_truncate(p->sb, &inode);
    inode.i_size_lo = 0;
    ext4_write_inode(p->sb, p->inode_num, &inode);
    p->file_size = 0;
    vn->size = 0;
    return 0;
}

static vnode_ops_t g_ext4_vnode_ops = {
    .lookup   = ext4_lookup,
    .create   = ext4_vn_create,
    .mkdir    = ext4_vn_mkdir,
    .unlink   = ext4_vn_unlink,
    .rmdir    = ext4_vn_rmdir,
    .rename   = ext4_vn_rename,
    .symlink  = ext4_vn_symlink,
    .readlink = ext4_readlink,
    .stat     = ext4_stat,
    .truncate = ext4_vn_truncate,
    .release  = ext4_release_vn,
};

/* ================================================================
 * File operations
 * ================================================================ */

static int ext4_fread(vfile_t *vf, char *buf, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (fc->is_dir) return -EISDIR;
    if (count == 0) return 0;

    size_t remaining = fc->file_size - fc->file_off;
    if (count > remaining) count = remaining;
    if (count == 0) return 0;

    ext4_inode_t inode;
    if (ext4_read_inode(fc->sb, fc->inode_num, &inode) < 0) return -EIO;

    uint32_t bs = fc->sb->block_size;
    size_t done = 0;
    while (done < count) {
        size_t foff = fc->file_off + done;
        uint32_t lblk = (uint32_t)(foff / bs);
        uint32_t loff = (uint32_t)(foff % bs);
        size_t chunk = bs - loff;
        if (chunk > count - done) chunk = count - done;

        uint64_t phys = ext4_block_map(fc->sb, &inode, lblk);
        if (!phys) {
            /* Sparse files read back as zero-filled holes, not EOF. */
            memset(buf + done, 0, chunk);
        } else {
            int r = bcache_read_bytes(fc->sb->bc, phys * bs + loff, buf + done, chunk);
            if (r < 0) break;
        }
        done += chunk;
    }

    fc->file_off += done;
    vf->offset = fc->file_off;
    return (int)done;
}

static int ext4_fwrite(vfile_t *vf, const char *buf, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (fc->is_dir) return -EISDIR;
    if (count == 0) return 0;

    ext4_inode_t inode;
    if (ext4_read_inode(fc->sb, fc->inode_num, &inode) < 0) return -EIO;

    uint32_t bs = fc->sb->block_size;
    size_t done = 0;

    while (done < count) {
        size_t foff = fc->file_off + done;
        uint32_t lblk = (uint32_t)(foff / bs);
        uint32_t loff = (uint32_t)(foff % bs);
        size_t chunk = bs - loff;
        if (chunk > count - done) chunk = count - done;

        uint64_t phys = ext4_block_map(fc->sb, &inode, lblk);
        if (!phys) {
            /* Need to allocate a new block */
            uint64_t nb = ext4_alloc_block(fc->sb);
            if (!nb) break;
            int gr = ext4_block_grow(fc->sb, &inode, lblk, nb);
            if (gr < 0) { ext4_free_block(fc->sb, nb); break; }
            phys = nb;
        }

        int r = bcache_write_bytes(fc->sb->bc, phys * bs + loff, buf + done, chunk);
        if (r < 0) break;
        done += chunk;
    }

    if (done > 0) {
        fc->file_off += done;
        if (fc->file_off > fc->file_size) {
            fc->file_size = fc->file_off;
            inode.i_size_lo = (uint32_t)fc->file_size;
            ext4_write_inode(fc->sb, fc->inode_num, &inode);
            if (vf->vnode && vf->vnode->fs_data) {
                ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vf->vnode->fs_data;
                fp->file_size = fc->file_size;
                vf->vnode->size = fc->file_size;
            }
        }
        vf->offset = fc->file_off;
    }
    return (int)done;
}

static long ext4_flseek(vfile_t *vf, long offset, int whence) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)fc->file_size + offset; break;
        default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    vf->offset   = (size_t)new_off;
    fc->file_off = (size_t)new_off;
    return new_off;
}

static int ext4_freaddir(vfile_t *vf, void *dirp, size_t count) {
    ext4_fctx_t *fc = (ext4_fctx_t *)vf->priv;
    if (!fc->is_dir) return -ENOTDIR;

    ext4_inode_t di;
    if (ext4_read_inode(fc->sb, fc->inode_num, &di) < 0) return -EIO;

    uint32_t bs = fc->sb->block_size;
    uint32_t total_blocks = (fc->file_size + bs - 1) / bs;
    char *out = (char *)dirp;
    size_t total = 0;

    for (uint32_t b = (uint32_t)(fc->dir_off / bs); b < total_blocks; b++) {
        uint64_t p = ext4_block_map(fc->sb, &di, b);
        if (!p) continue;
        char *blk = (char *)kmalloc(bs);
        if (!blk) break;
        if (bcache_read_bytes(fc->sb->bc, p * bs, blk, bs) < 0) { kfree(blk); break; }

        uint32_t start = (b == (uint32_t)(fc->dir_off / bs)) ? (uint32_t)(fc->dir_off % bs) : 0;
        uint32_t off = start;
        while (off < bs) {
            ext4_dir_entry_t *de = (ext4_dir_entry_t *)(blk + off);
            if (de->rec_len == 0) break;
            if (de->inode == 0) { off += de->rec_len; continue; }

            char fname[256];
            size_t nl = de->name_len;
            if (nl > 255) nl = 255;
            memcpy(fname, de->name, nl);
            fname[nl] = '\0';

            size_t reclen = offsetof(a20_dirent64_t, d_name) + nl + 1;
            reclen = (reclen + 7) & ~7UL;
            if (total + reclen > count) { kfree(blk); goto out; }

            a20_dirent64_t *dent = (a20_dirent64_t *)(out + total);
            dent->d_ino    = de->inode;
            dent->d_off    = (int64_t)(b * bs + off);
            dent->d_reclen = (uint16_t)reclen;
            dent->d_type   = (de->file_type == EXT4_FT_DIR) ? DT_DIR :
                             (de->file_type == EXT4_FT_SYMLINK) ? DT_LNK : DT_REG;
            memcpy(dent->d_name, fname, nl + 1);
            total += reclen;

            off += de->rec_len;
            fc->dir_off = b * bs + off;
        }
        kfree(blk);
    }

out:
    return (int)total;
}

static int ext4_fclose(vfile_t *vf) {
    if (vf->priv) { kfree(vf->priv); vf->priv = NULL; }
    return 0;
}

static vfile_ops_t g_ext4_fops = {
    .read    = ext4_fread,
    .write   = ext4_fwrite,
    .lseek   = ext4_flseek,
    .readdir = ext4_freaddir,
    .ioctl   = NULL,
    .close   = ext4_fclose,
};

/* ================================================================
 * VNode factory
 * ================================================================ */

static vnode_t *ext4_make_vnode(ext4_sb_info_t *sb, uint32_t ino, uint32_t sz,
                                 int type, vnode_t *parent) {
    /* The cache hook is currently disabled; keep the lookup call so the
     * make_vnode path stays self-contained if we ever reintroduce it. */
    vnode_t *cached = ext4_vnode_cache_lookup(sb, ino);
    if (cached) return cached;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino       = (uint64_t)ino;
    vn->type      = type;
    if (type == VFS_FT_DIR) vn->mode = S_IFDIR | 0755;
    else if (type == VFS_FT_SYMLINK) vn->mode = S_IFLNK | 0777;
    else vn->mode = S_IFREG | 0755;
    vn->size      = sz;
    vn->ref_count = 1;            /* 1 for the caller */
    vn->parent    = parent;
    if (parent) parent->ref_count++;
    vn->ops       = &g_ext4_vnode_ops;

    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)kmalloc(sizeof(ext4_vnode_priv_t));
    if (!fp) { kfree(vn); return NULL; }
    fp->sb        = sb;
    fp->inode_num = ino;
    fp->file_size = sz;
    fp->type      = type;
    vn->fs_data   = fp;

    ext4_vnode_cache_insert(sb, ino, vn);
    return vn;
}

/* ================================================================
 * Mount / Unmount
 * ================================================================ */

vnode_t *ext4_mount(bcache_t *bc) {
    ext4_superblock_t sb;
    if (bcache_read_bytes(bc, 1024, &sb, sizeof(sb)) < 0) {
        printf("[EXT4] Failed to read superblock\n");
        return NULL;
    }

    if (sb.s_magic != EXT4_DISK_MAGIC) {
        printf("[EXT4] Bad magic: 0x%x (expected 0x%x)\n", sb.s_magic, EXT4_DISK_MAGIC);
        return NULL;
    }

    uint32_t block_size = 1024 << sb.s_log_block_size;
    uint32_t desc_size = 32;
    if (sb.s_rev_level == EXT4_DYNAMIC_REV && sb.s_desc_size >= 32)
        desc_size = sb.s_desc_size;

    ext4_sb_info_t *esi = (ext4_sb_info_t *)kmalloc(sizeof(ext4_sb_info_t));
    if (!esi) {
        printf("[EXT4] Failed to allocate sb_info\n");
        return NULL;
    }
    memset(esi, 0, sizeof(*esi));

    esi->inodes_count = sb.s_inodes_count;

    uint32_t blocks_count = sb.s_blocks_count_lo;
    uint32_t groups = (blocks_count - sb.s_first_data_block + sb.s_blocks_per_group - 1)
                      / sb.s_blocks_per_group;

    esi->block_size   = block_size;
    esi->blocks_per_group = sb.s_blocks_per_group;
    esi->inodes_per_group = sb.s_inodes_per_group;
    esi->inode_size   = (sb.s_rev_level == EXT4_DYNAMIC_REV) ? sb.s_inode_size : 128;
    esi->first_data_block = sb.s_first_data_block;
    esi->groups_count = groups;
    esi->addr_per_block = block_size / 4;
    esi->desc_size     = desc_size;
    esi->s_feature_incompat = sb.s_feature_incompat;
    esi->s_feature_ro_compat = sb.s_feature_ro_compat;
    esi->bc           = bc;

    uint64_t gd_start;
    if (block_size == 1024)
        gd_start = 2048;
    else
        gd_start = (uint64_t)(sb.s_first_data_block + 1) * block_size;
    esi->block_group_desc_table_byte = gd_start;

    size_t gd_total = (size_t)groups * desc_size;
    esi->group_descs = (ext4_group_desc_t *)kmalloc(gd_total);
    if (!esi->group_descs) {
        printf("[EXT4] Failed to allocate group descriptors\n");
        kfree(esi);
        return NULL;
    }
    memset(esi->group_descs, 0, gd_total);
    if (bcache_read_bytes(bc, gd_start, esi->group_descs, gd_total) < 0) {
        printf("[EXT4] Failed to read group descriptors\n");
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    printf("[EXT4] Mounted: block_size=%u groups=%u inode_size=%u inodes/group=%u\n",
           block_size, groups, esi->inode_size, sb.s_inodes_per_group);

    ext4_inode_t root_inode;
    if (ext4_read_inode(esi, EXT4_ROOT_INO, &root_inode) < 0) {
        printf("[EXT4] Failed to read root inode\n");
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    vnode_t *root = ext4_make_vnode(esi, EXT4_ROOT_INO,
                                     root_inode.i_size_lo, VFS_FT_DIR, NULL);
    if (!root) {
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }
    root->parent = root;
    return root;
}

void ext4_unmount(vnode_t *root) {
    if (!root || !root->fs_data) return;
    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)root->fs_data;
    ext4_sb_info_t *esi = fp->sb;
    bcache_sync(esi->bc);
    if (root->ops && root->ops->release) root->ops->release(root);
    if (esi->group_descs) {
        kfree(esi->group_descs);
        esi->group_descs = NULL;
    }
    kfree(esi);
}

/* ================================================================
 * VFS open hook: create vfile for an ext4 vnode
 * ================================================================ */

vfile_t *ext4_open_vnode(vnode_t *vn, int flags) {
    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)vn->fs_data;
    ext4_fctx_t *fc = (ext4_fctx_t *)kmalloc(sizeof(ext4_fctx_t));
    if (!fc) return NULL;
    memset(fc, 0, sizeof(*fc));
    fc->sb        = fp->sb;
    fc->inode_num = fp->inode_num;
    uint64_t current_size = fp->file_size;
    ext4_inode_t dinode;
    if (ext4_read_inode(fp->sb, fp->inode_num, &dinode) == 0) {
        current_size = dinode.i_size_lo;
        fp->file_size = current_size;
        vn->size = current_size;
    }
    fc->file_size = current_size;
    fc->is_dir    = (fp->type == VFS_FT_DIR);
    fc->file_off  = (flags & O_APPEND) ? current_size : 0;
    fc->dir_off   = 0;

    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) { kfree(fc); return NULL; }
    memset(vf, 0, sizeof(*vf));
    vf->vnode     = vn;
    vn->ref_count++;
    vf->flags     = flags;
    vf->offset    = fc->file_off;
    vf->ref_count = 1;
    vf->ops       = &g_ext4_fops;
    vf->priv      = fc;
    return vf;
}
