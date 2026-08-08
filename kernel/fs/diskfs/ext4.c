#include "fs/ext4.h"
#include "fs/ext4_internal.h"
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
#include "core/perf.h"
#include "core/timekeeping.h"

/* ---- 64-bit inode size accessors ----
 * i_size_high sits in the static 128-byte inode area (offset 108) and is
 * already covered by ext4_read_inode()/ext4_write_inode(), so big (>4 GiB)
 * files work without enlarging the inode I/O size. */

/* ---- inode & extent cache helpers ---- */



uint64_t ext4_block_map_cached(ext4_fctx_t *fc, ext4_inode_t *inode,
                                       uint32_t lblk) {
    if (fc->ext_valid && lblk >= fc->ext_start &&
        lblk < fc->ext_start + fc->ext_len)
        return fc->ext_phys + (lblk - fc->ext_start);

    uint64_t phys = ext4_block_map(fc->sb, inode, lblk);

    if (phys && fc->ext_valid &&
        lblk == fc->ext_start + fc->ext_len &&
        phys == fc->ext_phys + fc->ext_len) {
        fc->ext_len++;
    } else if (phys) {
        fc->ext_start = lblk;
        fc->ext_len   = 1;
        fc->ext_phys  = phys;
        fc->ext_valid = 1;
    } else {
        fc->ext_valid = 0;
    }
    return phys;
}

/* ================================================================
 * Vnode lifecycle
 *
 * Ext4 keeps exactly one live vnode per inode through a strong-reference
 * cache (below).  The cache owns one reference per entry; unlink/rmdir
 * and rename-over remove the victim from the cache and mark it unlinked,
 * deferring block/inode reclamation to ext4_release_vn() so that files
 * can be unlinked while still open (standard POSIX/Linux semantics).
 * ================================================================ */

/* ================================================================
 * VNode cache (strong reference, one vnode per inode)
 *
 * Each cached entry owns one vnode reference, so a cached vnode never
 * reaches release().  unlink/rmdir/rename-remove pull the victim out of
 * the cache and mark it unlinked; the cluster/inode data is then freed
 * by ext4_release_vn() once the last reference (e.g. an open fd) drops.
 * Callers must hold sb->metadata_lock; remove transfers the cache-owned
 * reference to the caller, who must vnode_put it after dropping the lock.
 * ================================================================ */

#define EXT4_VCACHE_MAX 512
typedef struct {
    ext4_sb_info_t *sb;
    uint32_t ino;
    vnode_t *vn;
} ext4_vcache_ent_t;

static ext4_vcache_ent_t g_ext4_vcache[EXT4_VCACHE_MAX];
static spinlock_t g_ext4_vcache_lock = SPINLOCK_INIT;

vnode_t *ext4_vnode_cache_lookup(ext4_sb_info_t *sb, uint32_t ino) {
    uint64_t flags = spin_lock_irqsave(&g_ext4_vcache_lock);
    for (int i = 0; i < EXT4_VCACHE_MAX; i++) {
        if (g_ext4_vcache[i].vn && g_ext4_vcache[i].sb == sb &&
            g_ext4_vcache[i].ino == ino) {
            vnode_t *vn = g_ext4_vcache[i].vn;
            /* Pair the cache lookup with its caller reference while the
             * entry is protected.  Cache pruning can therefore never turn
             * a successful lookup into a stale pointer. */
            vnode_get(vn);
            spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
            return vn;
        }
    }
    spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
    return NULL;
}

void ext4_vnode_cache_insert(ext4_sb_info_t *sb, uint32_t ino, vnode_t *vn) {
    uint64_t flags = spin_lock_irqsave(&g_ext4_vcache_lock);
    for (int i = 0; i < EXT4_VCACHE_MAX; i++) {
        if (!g_ext4_vcache[i].vn) {
            vnode_get(vn);
            g_ext4_vcache[i].sb = sb;
            g_ext4_vcache[i].ino = ino;
            g_ext4_vcache[i].vn = vn;
            spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
}

vnode_t *ext4_vnode_cache_remove(ext4_sb_info_t *sb, uint32_t ino) {
    uint64_t flags = spin_lock_irqsave(&g_ext4_vcache_lock);
    for (int i = 0; i < EXT4_VCACHE_MAX; i++) {
        if (g_ext4_vcache[i].vn && g_ext4_vcache[i].sb == sb &&
            g_ext4_vcache[i].ino == ino) {
            vnode_t *vn = g_ext4_vcache[i].vn;
            g_ext4_vcache[i].sb = NULL;
            g_ext4_vcache[i].ino = 0;
            g_ext4_vcache[i].vn = NULL;
            spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
            return vn;
        }
    }
    spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);
    return NULL;
}

void ext4_vnode_cache_prune_all(void)
{
    vnode_t *held[64];
    int held_count;

    /* A child vnode owns a parent reference, so reclaim cache-only leaves in
     * repeated passes.  Releasing one layer can make its parent cache-only on
     * the next pass.  Vnodes with an open file, mapping, path-walk, dcache, or
     * mount reference remain untouched. */
    do {
        held_count = 0;
        uint64_t flags = spin_lock_irqsave(&g_ext4_vcache_lock);
        for (int i = 0; i < EXT4_VCACHE_MAX && held_count < 64; i++) {
            vnode_t *vn = g_ext4_vcache[i].vn;
            if (vn && vnode_ref_read(vn) == 1) {
                held[held_count++] = vn;
                g_ext4_vcache[i].sb = NULL;
                g_ext4_vcache[i].ino = 0;
                g_ext4_vcache[i].vn = NULL;
            }
        }
        spin_unlock_irqrestore(&g_ext4_vcache_lock, flags);

        /* release() can free memory and take the ext4 metadata mutex; never
         * invoke it while holding the short cache spinlock. */
        for (int i = 0; i < held_count; i++)
            vnode_put(held[i]);
    } while (held_count != 0);
}

/* ================================================================
 * Inode I/O
 * ================================================================ */

int ext4_read_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *out) {
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

int ext4_write_inode(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *inp) {
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

void ext4_writeback_gd(ext4_sb_info_t *sb, uint32_t group) {
    uint64_t off = sb->block_group_desc_table_byte + (uint64_t)group * sb->desc_size;
    uint32_t n = sb->desc_size < sizeof(ext4_group_desc_t) ?
                 sb->desc_size : sizeof(ext4_group_desc_t);
    bcache_write_bytes(sb->bc, off, &sb->group_descs[group], n);
}

static int ext4_read_group_descs(ext4_sb_info_t *sb)
{
    memset(sb->group_descs, 0,
           (size_t)sb->groups_count * sizeof(ext4_group_desc_t));
    uint32_t n = sb->desc_size < sizeof(ext4_group_desc_t) ?
                 sb->desc_size : sizeof(ext4_group_desc_t);
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        uint64_t off = sb->block_group_desc_table_byte +
                       (uint64_t)g * sb->desc_size;
        if (bcache_read_bytes(sb->bc, off, &sb->group_descs[g], n) < 0)
            return -EIO;
    }
    return 0;
}

static uint32_t ext4_bg_free_blocks(const ext4_group_desc_t *gd)
{
    return (uint32_t)gd->bg_free_blocks_count_lo |
           ((uint32_t)gd->bg_free_blocks_count_hi << 16);
}

static uint32_t ext4_bg_free_inodes(const ext4_group_desc_t *gd)
{
    return (uint32_t)gd->bg_free_inodes_count_lo |
           ((uint32_t)gd->bg_free_inodes_count_hi << 16);
}

static void ext4_bg_set_free_blocks(ext4_group_desc_t *gd, uint32_t count)
{
    gd->bg_free_blocks_count_lo = (uint16_t)count;
    gd->bg_free_blocks_count_hi = (uint16_t)(count >> 16);
}

static void ext4_bg_set_free_inodes(ext4_group_desc_t *gd, uint32_t count)
{
    gd->bg_free_inodes_count_lo = (uint16_t)count;
    gd->bg_free_inodes_count_hi = (uint16_t)(count >> 16);
}

int ext4_bitmap_alloc(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t max,
                      uint32_t start) {
    if (!sb || !max || bm_blk >= sb->blocks_count)
        return -1;
    uint32_t bitmap_bits = sb->block_size * 8;
    if (max > bitmap_bits)
        return -1;
    start %= max;

    uint8_t *buf = (uint8_t *)kmalloc(sb->block_size);
    if (!buf) return -1;
    if (bcache_read_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size) < 0)
        { kfree(buf); return -1; }
    for (uint32_t n = 0; n < max; n++) {
        uint32_t bit = start + n;
        if (bit >= max)
            bit -= max;
        if (!(buf[bit / 8] & (1U << (bit % 8)))) {
            buf[bit / 8] |= (1U << (bit % 8));
            if (bcache_write_bytes(sb->bc, bm_blk * sb->block_size,
                                   buf, sb->block_size) < 0) {
                a20_perf_add(A20_PERF_EXT4_BITMAP_PROBES, n + 1);
                kfree(buf);
                return -1;
            }
            a20_perf_add(A20_PERF_EXT4_BITMAP_PROBES, n + 1);
            kfree(buf); return (int)bit;
        }
    }
    a20_perf_add(A20_PERF_EXT4_BITMAP_PROBES, max);
    kfree(buf); return -1;
}

int ext4_bitmap_free(ext4_sb_info_t *sb, uint64_t bm_blk, uint32_t bit) {
    if (!sb || bm_blk >= sb->blocks_count ||
        bit >= sb->block_size * 8)
        return -EINVAL;
    uint8_t *buf = (uint8_t *)kmalloc(sb->block_size);
    if (!buf) return -ENOMEM;
    if (bcache_read_bytes(sb->bc, bm_blk * sb->block_size, buf, sb->block_size) < 0)
        { kfree(buf); return -EIO; }
    if (!(buf[bit / 8] & (1U << (bit % 8)))) {
        kfree(buf);
        return -EINVAL;
    }
    buf[bit / 8] &= ~(1U << (bit % 8));
    int r = bcache_write_bytes(sb->bc, bm_blk * sb->block_size,
                               buf, sb->block_size);
    kfree(buf);
    return r < 0 ? -EIO : 0;
}

static uint32_t ext4_group_block_count(ext4_sb_info_t *sb, uint32_t group)
{
    uint64_t first = (uint64_t)sb->first_data_block +
                     (uint64_t)group * sb->blocks_per_group;
    if (first >= sb->blocks_count)
        return 0;
    uint64_t remaining = sb->blocks_count - first;
    return remaining < sb->blocks_per_group ? (uint32_t)remaining :
                                              sb->blocks_per_group;
}

static uint32_t ext4_group_inode_count(ext4_sb_info_t *sb, uint32_t group)
{
    uint64_t first = (uint64_t)group * sb->inodes_per_group;
    if (first >= sb->inodes_count)
        return 0;
    uint64_t remaining = sb->inodes_count - first;
    return remaining < sb->inodes_per_group ? (uint32_t)remaining :
                                              sb->inodes_per_group;
}

static int ext4_validate_group_counts(ext4_sb_info_t *sb)
{
    for (uint32_t g = 0; g < sb->groups_count; g++) {
        uint32_t blocks = ext4_group_block_count(sb, g);
        uint32_t inodes = ext4_group_inode_count(sb, g);
        uint64_t block_bitmap =
            (uint64_t)sb->group_descs[g].bg_block_bitmap_lo |
            ((uint64_t)sb->group_descs[g].bg_block_bitmap_hi << 32);
        uint64_t inode_bitmap =
            (uint64_t)sb->group_descs[g].bg_inode_bitmap_lo |
            ((uint64_t)sb->group_descs[g].bg_inode_bitmap_hi << 32);
        uint64_t inode_table =
            (uint64_t)sb->group_descs[g].bg_inode_table_lo |
            ((uint64_t)sb->group_descs[g].bg_inode_table_hi << 32);
        uint64_t inode_table_bytes = (uint64_t)inodes * sb->inode_size;
        uint64_t inode_table_blocks =
            (inode_table_bytes + sb->block_size - 1) / sb->block_size;
        if (!blocks || !inodes ||
            block_bitmap < sb->first_data_block ||
            inode_bitmap < sb->first_data_block ||
            block_bitmap >= sb->blocks_count ||
            inode_bitmap >= sb->blocks_count ||
            inode_table < sb->first_data_block ||
            inode_table >= sb->blocks_count ||
            !inode_table_blocks ||
            inode_table_blocks > sb->blocks_count - inode_table ||
            ext4_bg_free_blocks(&sb->group_descs[g]) > blocks ||
            ext4_bg_free_inodes(&sb->group_descs[g]) > inodes)
            return -EINVAL;
    }
    return 0;
}

uint64_t ext4_alloc_block(ext4_sb_info_t *sb) {
    /* Allocate the zeroing buffer before taking the allocator mutex. */
    char *zero_buf = (char *)kmalloc(sb->block_size);
    if (zero_buf)
        memset(zero_buf, 0, sb->block_size);

    mutex_lock(&sb->alloc_lock);
    uint64_t group_probes = 0;
    for (uint32_t n = 0; n < sb->groups_count; n++) {
        group_probes++;
        uint32_t g = sb->block_group_rotor + n;
        if (g >= sb->groups_count)
            g -= sb->groups_count;
        uint32_t free_blocks = ext4_bg_free_blocks(&sb->group_descs[g]);
        if (free_blocks == 0) continue;
        uint32_t valid = ext4_group_block_count(sb, g);
        if (!valid)
            continue;
        uint64_t bm = (uint64_t)sb->group_descs[g].bg_block_bitmap_lo |
                      ((uint64_t)sb->group_descs[g].bg_block_bitmap_hi << 32);
        int bit = ext4_bitmap_alloc(sb, bm, valid,
                                    sb->block_alloc_hints[g]);
        if (bit < 0) continue;
        sb->block_alloc_hints[g] = ((uint32_t)bit + 1) % valid;
        sb->block_group_rotor = g;
        ext4_bg_set_free_blocks(&sb->group_descs[g], free_blocks - 1);
        ext4_writeback_gd(sb, g);
        uint64_t phys = (uint64_t)sb->first_data_block +
                        (uint64_t)g * sb->blocks_per_group + bit;
        a20_perf_add(A20_PERF_EXT4_GROUP_PROBES, group_probes);
        mutex_unlock(&sb->alloc_lock);
        if (zero_buf)
            bcache_write_bytes(sb->bc, phys * sb->block_size, zero_buf, sb->block_size);
        if (zero_buf) kfree(zero_buf);
        return phys;
    }
    a20_perf_add(A20_PERF_EXT4_GROUP_PROBES, group_probes);
    mutex_unlock(&sb->alloc_lock);
    if (zero_buf) kfree(zero_buf);
    return 0;
}

void ext4_free_block(ext4_sb_info_t *sb, uint64_t phys) {
    mutex_lock(&sb->alloc_lock);
    if (phys < sb->first_data_block || phys >= sb->blocks_count) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint32_t rel = (uint32_t)(phys - sb->first_data_block);
    uint32_t g = rel / sb->blocks_per_group;
    uint32_t bit = rel % sb->blocks_per_group;
    if (g >= sb->groups_count) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint32_t valid = ext4_group_block_count(sb, g);
    if (bit >= valid) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint64_t bm = (uint64_t)sb->group_descs[g].bg_block_bitmap_lo |
                  ((uint64_t)sb->group_descs[g].bg_block_bitmap_hi << 32);
    uint32_t free_blocks = ext4_bg_free_blocks(&sb->group_descs[g]);
    if (free_blocks >= valid) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    if (ext4_bitmap_free(sb, bm, bit) < 0) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    if (sb->block_alloc_hints)
        sb->block_alloc_hints[g] = bit;
    ext4_bg_set_free_blocks(&sb->group_descs[g], free_blocks + 1);
    ext4_writeback_gd(sb, g);
    mutex_unlock(&sb->alloc_lock);
}

uint32_t ext4_alloc_inode(ext4_sb_info_t *sb) {
    mutex_lock(&sb->alloc_lock);
    uint64_t group_probes = 0;
    for (uint32_t n = 0; n < sb->groups_count; n++) {
        group_probes++;
        uint32_t g = sb->inode_group_rotor + n;
        if (g >= sb->groups_count)
            g -= sb->groups_count;
        uint32_t free_inodes = ext4_bg_free_inodes(&sb->group_descs[g]);
        if (free_inodes == 0) continue;
        uint32_t valid = ext4_group_inode_count(sb, g);
        if (!valid)
            continue;
        uint64_t bm = (uint64_t)sb->group_descs[g].bg_inode_bitmap_lo |
                      ((uint64_t)sb->group_descs[g].bg_inode_bitmap_hi << 32);
        int bit = ext4_bitmap_alloc(sb, bm, valid,
                                    sb->inode_alloc_hints[g]);
        if (bit < 0) continue;
        sb->inode_alloc_hints[g] = ((uint32_t)bit + 1) % valid;
        sb->inode_group_rotor = g;
        ext4_bg_set_free_inodes(&sb->group_descs[g], free_inodes - 1);
        ext4_writeback_gd(sb, g);
        uint32_t ino = g * sb->inodes_per_group + bit + 1;
        a20_perf_add(A20_PERF_EXT4_GROUP_PROBES, group_probes);
        mutex_unlock(&sb->alloc_lock);
        return ino;
    }
    a20_perf_add(A20_PERF_EXT4_GROUP_PROBES, group_probes);
    mutex_unlock(&sb->alloc_lock);
    return 0;
}

void ext4_free_inode(ext4_sb_info_t *sb, uint32_t ino) {
    mutex_lock(&sb->alloc_lock);
    if (!ino || ino > sb->inodes_count) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint32_t g = (ino - 1) / sb->inodes_per_group;
    uint32_t bit = (ino - 1) % sb->inodes_per_group;
    if (g >= sb->groups_count) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint32_t valid = ext4_group_inode_count(sb, g);
    if (bit >= valid) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    uint64_t bm = (uint64_t)sb->group_descs[g].bg_inode_bitmap_lo |
                  ((uint64_t)sb->group_descs[g].bg_inode_bitmap_hi << 32);
    uint32_t free_inodes = ext4_bg_free_inodes(&sb->group_descs[g]);
    if (free_inodes >= valid) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    if (ext4_bitmap_free(sb, bm, bit) < 0) {
        mutex_unlock(&sb->alloc_lock);
        return;
    }
    if (sb->inode_alloc_hints)
        sb->inode_alloc_hints[g] = bit;
    ext4_bg_set_free_inodes(&sb->group_descs[g], free_inodes + 1);
    ext4_writeback_gd(sb, g);
    mutex_unlock(&sb->alloc_lock);
}

/* ================================================================
 * Extent tree
 * ================================================================ */

uint64_t ext4_extent_leaf_search(ext4_extent_t *ex, int cnt, uint32_t lblk) {
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

uint64_t ext4_extent_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
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

int ext4_extent_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
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

void ext4_extent_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
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

/* ---- Partial extent truncate (collect → rebuild) ----
 * ext4_vn_truncate() used to free blocks only for size==0, leaking every
 * block beyond a non-zero EOF.  To reclaim them safely we flatten the whole
 * extent tree (depth 0/1, the shapes this driver itself creates), drop every
 * extent at/after the new logical EOF block, free those physical blocks, and
 * rebuild a fresh tree from the survivors.  Trees deeper than depth 1 are
 * left untouched (blocks preserved, not corrupted) — partial truncate then
 * merely resizes, matching the previous behaviour. */

int ext4_extent_collect(ext4_sb_info_t *sb, const ext4_inode_t *inode,
                               ext4_flatext_t *out, int max) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr; memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) return 0;
    int cnt = 0;

    if (hdr.eh_depth == 0) {
        int n = hdr.eh_entries; if (n > 4) n = 4;
        ext4_extent_t ext[4];
        memcpy(ext, raw + sizeof(hdr), n * sizeof(ext4_extent_t));
        for (int i = 0; i < n && cnt < max; i++) {
            uint16_t len = ext[i].ee_len; if (len > 0x8000) len -= 0x8000;
            out[cnt].start = ext[i].ee_block;
            out[cnt].len   = len;
            out[cnt].phys  = (uint64_t)ext[i].ee_start_lo |
                             ((uint64_t)ext[i].ee_start_hi << 32);
            cnt++;
        }
        return cnt;
    }

    ext4_extent_idx_t idx[4];
    int ni = hdr.eh_entries; if (ni > 4) ni = 4;
    memcpy(idx, raw + sizeof(hdr), ni * sizeof(ext4_extent_idx_t));
    for (int i = 0; i < ni; i++) {
        uint64_t lb = (uint64_t)idx[i].ei_leaf_lo | ((uint64_t)idx[i].ei_leaf_hi << 32);
        char *b = (char *)kmalloc(sb->block_size);
        if (!b) break;
        if (bcache_read_bytes(sb->bc, lb * sb->block_size, b, sb->block_size) < 0)
            { kfree(b); break; }
        ext4_extent_header_t lh; memcpy(&lh, b, sizeof(lh));
        if (lh.eh_magic == EXT4_EXT_MAGIC) {
            ext4_extent_t *ep = (ext4_extent_t *)(b + sizeof(lh));
            for (int j = 0; j < lh.eh_entries && cnt < max; j++) {
                uint16_t len = ep[j].ee_len; if (len > 0x8000) len -= 0x8000;
                out[cnt].start = ep[j].ee_block;
                out[cnt].len   = len;
                out[cnt].phys  = (uint64_t)ep[j].ee_start_lo |
                                 ((uint64_t)ep[j].ee_start_hi << 32);
                cnt++;
            }
        }
        kfree(b);
    }
    return cnt;
}

/* Drop old depth-0/1 extent tree blocks (leaf + index blocks). */
void ext4_extent_free_tree(ext4_sb_info_t *sb, const ext4_inode_t *inode) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr; memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) return;

    if (hdr.eh_depth == 0) return;

    ext4_extent_idx_t idx[4];
    int ni = hdr.eh_entries; if (ni > 4) ni = 4;
    memcpy(idx, raw + sizeof(hdr), ni * sizeof(ext4_extent_idx_t));
    for (int i = 0; i < ni; i++) {
        uint64_t lb = (uint64_t)idx[i].ei_leaf_lo | ((uint64_t)idx[i].ei_leaf_hi << 32);
        ext4_free_block(sb, lb);
    }
}

int ext4_extent_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                   uint32_t lblk) {
    uint8_t *raw = (uint8_t *)inode + offsetof(ext4_inode_t, i_block);
    ext4_extent_header_t hdr; memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.eh_magic != EXT4_EXT_MAGIC || hdr.eh_entries == 0) return 0;

    /* Depth > 1: not a shape we can safely rebuild; leave untouched. */
    if (hdr.eh_depth > 1) return 0;

    ext4_flatext_t *all = (ext4_flatext_t *)kmalloc(sizeof(ext4_flatext_t) * EXT4_MAX_FLATEXT);
    if (!all) return -ENOMEM;

    int n = ext4_extent_collect(sb, inode, all, EXT4_MAX_FLATEXT);
    if (n <= 0) { kfree(all); return 0; }

    /* First pass: compute the surviving extents (shrunk at lblk) without
     * freeing anything yet, so an allocation failure below cannot leave the
     * old tree referencing freed blocks.  Freed data blocks are remembered
     * in the survivors' space (drop marker) and reclaimed only after the new
     * tree has been fully staged. */
    int keep = 0;
    for (int i = 0; i < n; i++) {
        uint32_t s = all[i].start, len = all[i].len;
        if (s >= lblk)
            continue;                    /* whole extent goes away */
        if (s + len > lblk) {
            all[i].len = lblk - s;       /* keep the head below lblk */
            all[keep++] = all[i];
        } else {
            all[keep++] = all[i];        /* fully below lblk */
        }
    }

    /* ---- stage the new tree ---- */
    uint32_t epb = EXT_PER_BLK(sb->block_size);
    uint64_t leaves[4] = {0, 0, 0, 0};
    ext4_extent_idx_t idx[4];
    int nleaves = 0;
    char *lbuf = NULL;

    if (keep > 0 && keep > 4) {
        /* depth-1 rebuild needs leaf blocks; allocate+write them now. */
        if (keep > 4 * (int)epb) { kfree(all); return -ENOSPC; }
        nleaves = (keep + (int)epb - 1) / (int)epb;
        lbuf = (char *)kmalloc(sb->block_size);
        if (!lbuf) { kfree(all); return -ENOMEM; }
        int li = 0, in_leaf = 0;
        for (int i = 0; i < keep; i++) {
            if (in_leaf == 0) {
                uint64_t nl = ext4_alloc_block(sb);
                if (!nl) goto stage_fail;
                leaves[li] = nl;
                memset(lbuf, 0, sb->block_size);
                ext4_extent_header_t lh; lh.eh_magic = EXT4_EXT_MAGIC;
                lh.eh_entries = 0; lh.eh_max = (uint16_t)epb; lh.eh_depth = 0;
                lh.eh_generation = 0;
                memcpy(lbuf, &lh, sizeof(lh));
                idx[li].ei_block   = all[i].start;
                idx[li].ei_leaf_lo = (uint32_t)(nl & 0xffffffffu);
                idx[li].ei_leaf_hi = (uint16_t)(nl >> 32);
                idx[li].ei_unused  = 0;
            }
            ext4_extent_t *ep = (ext4_extent_t *)(lbuf + sizeof(ext4_extent_header_t));
            ep[in_leaf].ee_block    = all[i].start;
            ep[in_leaf].ee_len      = (uint16_t)all[i].len;
            ep[in_leaf].ee_start_hi = (uint16_t)(all[i].phys >> 32);
            ep[in_leaf].ee_start_lo = (uint32_t)(all[i].phys & 0xffffffffu);
            in_leaf++;
            ((ext4_extent_header_t *)lbuf)->eh_entries = (uint16_t)in_leaf;
            if (in_leaf == (int)epb || i == keep - 1) {
                if (bcache_write_bytes(sb->bc, leaves[li] * sb->block_size,
                                       lbuf, sb->block_size) < 0)
                    goto stage_fail;
                li++;
                in_leaf = 0;
            }
        }
        kfree(lbuf); lbuf = NULL;
        if (li != nleaves) goto stage_fail;
    }

    /* ---- commit: free the reclaimed blocks, then swap in the new tree ---- */
    for (int i = 0; i < n; i++) {
        uint32_t s = all[i].start, len = all[i].len;
        uint64_t p = all[i].phys;
        if (s >= lblk) {
            for (uint32_t j = 0; j < len; j++) ext4_free_block(sb, p + j);
        } else if (s + len > lblk) {
            uint32_t tail = s + len - lblk;
            for (uint32_t j = 0; j < tail; j++) ext4_free_block(sb, p + len - tail + j);
        }
    }

    ext4_extent_free_tree(sb, inode);

    if (keep == 0) {
        ext4_extent_header_t rst; rst.eh_magic = EXT4_EXT_MAGIC;
        rst.eh_entries = 0; rst.eh_max = 4; rst.eh_depth = 0; rst.eh_generation = 0;
        memcpy(raw, &rst, sizeof(rst));
        kfree(all);
        return 0;
    }

    if (keep <= 4) {
        ext4_extent_header_t nh; nh.eh_magic = EXT4_EXT_MAGIC;
        nh.eh_entries = (uint16_t)keep; nh.eh_max = 4; nh.eh_depth = 0;
        nh.eh_generation = 0;
        memcpy(raw, &nh, sizeof(nh));
        ext4_extent_t ext[4];
        for (int i = 0; i < keep; i++) {
            ext[i].ee_block    = all[i].start;
            ext[i].ee_len      = (uint16_t)all[i].len;
            ext[i].ee_start_hi = (uint16_t)(all[i].phys >> 32);
            ext[i].ee_start_lo = (uint32_t)(all[i].phys & 0xffffffffu);
        }
        memcpy(raw + sizeof(nh), ext, keep * sizeof(ext4_extent_t));
        kfree(all);
        return 0;
    }

    /* keep > 4: install the depth-1 tree staged above. */
    {
        ext4_extent_header_t nh; nh.eh_magic = EXT4_EXT_MAGIC;
        nh.eh_entries = (uint16_t)nleaves; nh.eh_max = 4; nh.eh_depth = 1;
        nh.eh_generation = 0;
        memcpy(raw, &nh, sizeof(nh));
        memcpy(raw + sizeof(nh), idx, nleaves * sizeof(ext4_extent_idx_t));
    }
    kfree(all);
    return 0;

stage_fail:
    /* New tree not yet installed and nothing was freed: undo the freshly
     * allocated leaf blocks and leave the old tree intact. */
    if (lbuf) kfree(lbuf);
    for (int i = 0; i < 4; i++)
        if (leaves[i]) ext4_free_block(sb, leaves[i]);
    kfree(all);
    return -EIO;
}

/* ================================================================
 * Indirect block mapping
 * ================================================================ */

uint64_t ext4_indirect_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
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

int ext4_indirect_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
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

void ext4_indirect_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
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

/* ---- Partial indirect truncate (free blocks at/after lblk) ----
 * Mirrors ext4_indirect_truncate but only reclaims the range starting at
 * logical block lblk, keeping everything below it intact.  Handles the
 * 12 direct + single + double indirect levels the writer can create. */
void ext4_indirect_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                      uint32_t lblk) {
    uint32_t b[15]; memcpy(b, inode->i_block.i_data.i_block, sizeof(b));
    uint32_t apb = sb->addr_per_block;
    uint32_t single_start = 12;
    uint32_t double_start = 12 + apb;

    if (lblk <= single_start) {
        /* Free direct blocks from lblk on, then the whole indirect trees. */
        for (uint32_t i = lblk; i < 12; i++) if (b[i]) ext4_free_block(sb, b[i]);
        if (b[12]) {
            uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
            if (ind) {
                bcache_read_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
                for (uint32_t i = 0; i < apb; i++) if (ind[i]) ext4_free_block(sb, ind[i]);
                kfree(ind);
            }
            ext4_free_block(sb, b[12]);
        }
        if (b[13]) {
            uint32_t *di = (uint32_t *)kmalloc(sb->block_size);
            if (di) {
                bcache_read_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
                for (uint32_t i = 0; i < apb; i++) if (di[i]) {
                    uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
                    if (ind) {
                        bcache_read_bytes(sb->bc, (uint64_t)di[i] * sb->block_size, ind, sb->block_size);
                        for (uint32_t j = 0; j < apb; j++) if (ind[j]) ext4_free_block(sb, ind[j]);
                        kfree(ind);
                    }
                    ext4_free_block(sb, di[i]);
                }
                kfree(di);
            }
            ext4_free_block(sb, b[13]);
        }
        for (uint32_t i = lblk; i < 15; i++) b[i] = 0;
        memcpy(inode->i_block.i_data.i_block, b, sizeof(b));
        return;
    }

    if (lblk < double_start) {
        /* Keep the single indirect block, zero+free its tail entries, and
         * drop the double indirect tree entirely. */
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
        if (ind) {
            bcache_read_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
            for (uint32_t i = lblk - single_start; i < apb; i++)
                if (ind[i]) { ext4_free_block(sb, ind[i]); ind[i] = 0; }
            bcache_write_bytes(sb->bc, (uint64_t)b[12] * sb->block_size, ind, sb->block_size);
            kfree(ind);
        }
        if (b[13]) {
            uint32_t *di = (uint32_t *)kmalloc(sb->block_size);
            if (di) {
                bcache_read_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
                for (uint32_t i = 0; i < apb; i++) if (di[i]) {
                    uint32_t *ind2 = (uint32_t *)kmalloc(sb->block_size);
                    if (ind2) {
                        bcache_read_bytes(sb->bc, (uint64_t)di[i] * sb->block_size, ind2, sb->block_size);
                        for (uint32_t j = 0; j < apb; j++) if (ind2[j]) ext4_free_block(sb, ind2[j]);
                        kfree(ind2);
                    }
                    ext4_free_block(sb, di[i]);
                }
                kfree(di);
            }
            ext4_free_block(sb, b[13]);
            b[13] = 0;
        }
        b[14] = 0;
        memcpy(inode->i_block.i_data.i_block, b, sizeof(b));
        return;
    }

    /* lblk >= double_start: keep direct + single, trim the double indirect
     * tree from the offset (lblk - double_start). */
    uint32_t rem = lblk - double_start;
    if (!b[13]) return;
    uint32_t *di = (uint32_t *)kmalloc(sb->block_size);
    if (!di) return;
    if (bcache_read_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size) < 0)
        { kfree(di); return; }
    uint32_t first_ii = rem / apb;
    for (uint32_t i = first_ii; i < apb; i++) {
        if (!di[i]) continue;
        uint32_t *ind = (uint32_t *)kmalloc(sb->block_size);
        if (!ind) continue;
        if (bcache_read_bytes(sb->bc, (uint64_t)di[i] * sb->block_size, ind, sb->block_size) < 0)
            { kfree(ind); continue; }
        uint32_t j0 = (i == first_ii) ? (rem % apb) : 0;
        for (uint32_t j = j0; j < apb; j++)
            if (ind[j]) { ext4_free_block(sb, ind[j]); ind[j] = 0; }
        if (j0 == 0) {
            bcache_write_bytes(sb->bc, (uint64_t)di[i] * sb->block_size, ind, sb->block_size);
            kfree(ind);
            ext4_free_block(sb, di[i]);
            di[i] = 0;
        } else {
            bcache_write_bytes(sb->bc, (uint64_t)di[i] * sb->block_size, ind, sb->block_size);
            kfree(ind);
        }
    }
    bcache_write_bytes(sb->bc, (uint64_t)b[13] * sb->block_size, di, sb->block_size);
    kfree(di);
    b[14] = 0;
    memcpy(inode->i_block.i_data.i_block, b, sizeof(b));
}

/* ---- generic dispatch ---- */

uint64_t ext4_block_map(ext4_sb_info_t *sb, ext4_inode_t *inode, uint32_t lblk) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext4_extent_map(sb, inode, lblk);
    return ext4_indirect_map(sb, inode, lblk);
}
int ext4_block_grow(ext4_sb_info_t *sb, ext4_inode_t *inode,
                            uint32_t lblk, uint64_t phys) {
    if (inode->i_flags & EXT4_EXTENTS_FL) return ext4_extent_grow(sb, inode, lblk, phys);
    return ext4_indirect_grow(sb, inode, lblk, phys);
}
void ext4_block_truncate(ext4_sb_info_t *sb, ext4_inode_t *inode) {
    if (inode->i_flags & EXT4_EXTENTS_FL) ext4_extent_truncate(sb, inode);
    else ext4_indirect_truncate(sb, inode);
}

/* Free every block at/after logical block lblk (partial truncate).  Keeps
 * the blocks below lblk, reclaiming the rest; used by truncate() on non-zero
 * new sizes so blocks beyond EOF are not leaked. */
void ext4_block_truncate_at(ext4_sb_info_t *sb, ext4_inode_t *inode,
                                   uint32_t lblk) {
    if (inode->i_flags & EXT4_EXTENTS_FL)
        ext4_extent_truncate_at(sb, inode, lblk);
    else
        ext4_indirect_truncate_at(sb, inode, lblk);
}

/* ================================================================
 * Directory helpers
 * ================================================================ */





/* ================================================================
 * Shared inode removal helper (used by unlink and rename)
 * ================================================================ */


/* ================================================================
 * VNode operations
 * ================================================================ */



























vnode_ops_t g_ext4_vnode_ops = {
    .lookup   = ext4_lookup,
    .create   = ext4_vn_create,
    .mkdir    = ext4_vn_mkdir,
    .unlink   = ext4_vn_unlink,
    .rmdir    = ext4_vn_rmdir,
    .rename   = ext4_vn_rename,
    .link     = ext4_vn_link,
    .symlink  = ext4_vn_symlink,
    .readlink = ext4_readlink,
    .stat     = ext4_stat,
    .statfs   = ext4_vn_statfs,
    .truncate = ext4_vn_truncate,
    .readpage = ext4_vn_readpage,
    .writepage = ext4_vn_writepage,
    .chmod    = ext4_vn_chmod,
    .chown    = ext4_vn_chown,
    .open     = ext4_open_vnode,
    .release  = ext4_release_vn,
};

/* ================================================================
 * File operations
 * ================================================================ */











/* ================================================================
 * VNode factory
 * ================================================================ */

vnode_t *ext4_make_vnode(ext4_sb_info_t *sb, uint32_t ino, uint32_t sz,
                                 int type, vnode_t *parent) {
    /* Caller holds sb->metadata_lock: reuse the cached vnode so an inode
     * has exactly one live vnode.  The in-memory file_size is authoritative
     * while the vnode is alive (the on-disk inode size lags behind until
     * writeback), so a cache hit must not overwrite it. */
    vnode_t *cached = ext4_vnode_cache_lookup(sb, ino);
    if (cached)
        return cached;

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino       = (uint64_t)ino;
    vn->type      = type;
    ext4_inode_t inode;
    if (ext4_read_inode(sb, ino, &inode) == 0 && inode.i_mode) {
        vn->mode = inode.i_mode;
        vn->uid = inode.i_uid;
        vn->gid = inode.i_gid;
    } else if (type == VFS_FT_DIR) {
        vn->mode = S_IFDIR | 0755;
    } else if (type == VFS_FT_SYMLINK) {
        vn->mode = S_IFLNK | 0777;
    } else {
        vn->mode = S_IFREG | 0755;
    }
    vn->size      = sz;
    vnode_ref_init(vn, 1);            /* 1 for the caller */
    vn->parent    = parent;
    if (parent) vnode_get(parent);
    if (parent)
        vn->mnt = parent->mnt;        /* inherit mount for link()/dcache */
    vn->ops       = &g_ext4_vnode_ops;

    ext4_vnode_priv_t *fp = (ext4_vnode_priv_t *)kmalloc(sizeof(ext4_vnode_priv_t));
    if (!fp) {
        vnode_put(vn);
        return NULL;
    }
    fp->sb        = sb;
    fp->inode_num = ino;
    fp->file_size = sz;
    fp->type      = type;
    fp->unlinked  = 0;
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

    /* ---- fail-closed feature gate ----
     * This driver understands extents, 64-bit block counts, flex_bg,
     * linear directory scans and the narrow checksummed JBD2 recovery path
     * implemented in ext4_journal.c.  It cannot safely read meta_bg,
     * bigalloc, inline-data, casefold, encryption or MMP
     * filesystems.  Refuse to mount instead of silently misreading the
     * image (mirrors how Linux rejects unknown incompatible features with a
     * clear error).
     *
     * CSUM_SEED is benign here: it only changes how the (unchecked) metadata
     * checksum seed is derived, so it is allowed.  RO-compat flags
     * (sparse_super, large_file, gdt_csum, dir_nlink, extra_isize,
     * htree-index) are ubiquitous on modern mkfs.ext4 images and are safe:
     * sparse_super/htree do not change the block layout the linear walk
     * depends on, and csums are simply not written back.  They must NOT be
     * rejected, otherwise ordinary Linux-formatted images would refuse to
     * mount (a regression). */
    {
        uint32_t ino = sb.s_feature_incompat;
        uint32_t unsupported_incompat = ino & ~(EXT4_FEATURE_INCOMPAT_FILETYPE |
                                                EXT4_FEATURE_INCOMPAT_EXTENTS |
                                                EXT4_FEATURE_INCOMPAT_64BIT |
                                                EXT4_FEATURE_INCOMPAT_FLEX_BG |
                                                EXT4_FEATURE_INCOMPAT_CSUM_SEED |
                                                EXT4_FEATURE_INCOMPAT_RECOVER);
        if (unsupported_incompat) {
            printf("[EXT4] Unsupported incompat features: 0x%x "
                   "(unsupported: 0x%x)\n", ino, unsupported_incompat);
            return NULL;
        }
    }

    if (sb.s_log_block_size > 6 || !sb.s_blocks_per_group ||
        !sb.s_inodes_per_group || !sb.s_inodes_count) {
        printf("[EXT4] Invalid superblock geometry\n");
        return NULL;
    }
    uint32_t expected_first_data_block = sb.s_log_block_size == 0 ? 1 : 0;
    if (sb.s_first_data_block != expected_first_data_block) {
        printf("[EXT4] Invalid first data block for block size\n");
        return NULL;
    }
    if (sb.s_log_cluster_size != sb.s_log_block_size) {
        printf("[EXT4] Bigalloc cluster geometry is unsupported\n");
        return NULL;
    }

    uint32_t block_size = 1024U << sb.s_log_block_size;
    uint32_t desc_size = 32;
    if (sb.s_rev_level == EXT4_DYNAMIC_REV && sb.s_desc_size >= 32)
        desc_size = sb.s_desc_size;

    uint32_t inode_size = sb.s_rev_level == EXT4_DYNAMIC_REV ?
                          sb.s_inode_size : EXT4_INODE_SIZE_STATIC;
    if (inode_size < EXT4_INODE_SIZE_STATIC || inode_size > block_size ||
        (inode_size & (inode_size - 1))) {
        printf("[EXT4] Invalid inode size: %u\n", inode_size);
        return NULL;
    }
    if ((sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) &&
        (sb.s_rev_level != EXT4_DYNAMIC_REV || sb.s_desc_size < 64)) {
        printf("[EXT4] 64-bit filesystem requires 64-byte descriptors\n");
        return NULL;
    }
    if (!(sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) &&
        (sb.s_blocks_count_hi || sb.s_r_blocks_count_hi)) {
        printf("[EXT4] High block counts without 64-bit feature\n");
        return NULL;
    }

    uint64_t blocks_count = (uint64_t)sb.s_blocks_count_lo |
                            ((uint64_t)sb.s_blocks_count_hi << 32);
    uint64_t reserved_blocks = (uint64_t)sb.s_r_blocks_count_lo |
                               ((uint64_t)sb.s_r_blocks_count_hi << 32);
    if (blocks_count <= sb.s_first_data_block || desc_size > block_size ||
        (desc_size & 7) || reserved_blocks > blocks_count ||
        sb.s_blocks_per_group > block_size * 8 ||
        sb.s_inodes_per_group > block_size * 8 ||
        (desc_size < sizeof(ext4_group_desc_t) &&
         (sb.s_blocks_per_group > 0xffffU ||
          sb.s_inodes_per_group > 0xffffU))) {
        printf("[EXT4] Invalid block-group geometry\n");
        return NULL;
    }
    uint64_t data_blocks = blocks_count - sb.s_first_data_block;
    uint64_t groups64 = data_blocks / sb.s_blocks_per_group +
                        (data_blocks % sb.s_blocks_per_group != 0);
    uint64_t inode_groups = sb.s_inodes_count / sb.s_inodes_per_group +
                            (sb.s_inodes_count % sb.s_inodes_per_group != 0);
    if (!groups64 || groups64 > 0xffffffffULL || inode_groups != groups64) {
        printf("[EXT4] Inconsistent block-group counts\n");
        return NULL;
    }
    uint32_t groups = (uint32_t)groups64;

    uint64_t gd_start = block_size == 1024 ? 2048ULL :
                        ((uint64_t)sb.s_first_data_block + 1) * block_size;
    if (blocks_count > ~0ULL / block_size) {
        printf("[EXT4] Filesystem byte size overflows\n");
        return NULL;
    }
    uint64_t fs_bytes = blocks_count * block_size;
    uint64_t gd_bytes = groups64 * desc_size;
    if (gd_start > fs_bytes || gd_bytes > fs_bytes - gd_start ||
        groups64 > (uint64_t)((size_t)-1) / sizeof(ext4_group_desc_t)) {
        printf("[EXT4] Group descriptor table exceeds filesystem\n");
        return NULL;
    }
    if (bc->dev && bc->dev->capacity && bc->dev->sector_size &&
        bc->dev->capacity <= ~0ULL / bc->dev->sector_size &&
        fs_bytes > bc->dev->capacity * bc->dev->sector_size) {
        printf("[EXT4] Filesystem exceeds block device capacity\n");
        return NULL;
    }

    ext4_sb_info_t *esi = (ext4_sb_info_t *)kmalloc(sizeof(ext4_sb_info_t));
    if (!esi) {
        printf("[EXT4] Failed to allocate sb_info\n");
        return NULL;
    }
    memset(esi, 0, sizeof(*esi));
    mutex_init(&esi->alloc_lock);
    mutex_init(&esi->metadata_lock);

    esi->inodes_count = sb.s_inodes_count;

    esi->blocks_count = blocks_count;
    esi->reserved_blocks_count = reserved_blocks;
    esi->block_size   = block_size;
    esi->blocks_per_group = sb.s_blocks_per_group;
    esi->inodes_per_group = sb.s_inodes_per_group;
    esi->inode_size   = inode_size;
    esi->first_data_block = sb.s_first_data_block;
    esi->groups_count = groups;
    esi->addr_per_block = block_size / 4;
    esi->desc_size     = desc_size;
    esi->s_feature_incompat = sb.s_feature_incompat;
    esi->s_feature_ro_compat = sb.s_feature_ro_compat;
    esi->bc           = bc;

    esi->block_group_desc_table_byte = gd_start;

    size_t gd_total = (size_t)groups * sizeof(ext4_group_desc_t);
    esi->group_descs = (ext4_group_desc_t *)kmalloc(gd_total);
    if (!esi->group_descs) {
        printf("[EXT4] Failed to allocate group descriptors\n");
        kfree(esi);
        return NULL;
    }
    if (ext4_read_group_descs(esi) < 0) {
        printf("[EXT4] Failed to read group descriptors\n");
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    if (sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
        if (ext4_journal_recover(esi, &sb) < 0) {
            printf("[EXT4] Refusing mount after journal recovery failure\n");
            kfree(esi->group_descs);
            kfree(esi);
            return NULL;
        }
        /* Journal replay can replace the primary group descriptor table.
         * Refresh the in-memory copy before allocation or inode lookup. */
        if (ext4_read_group_descs(esi) < 0) {
            printf("[EXT4] Failed to refresh group descriptors after recovery\n");
            kfree(esi->group_descs);
            kfree(esi);
            return NULL;
        }
    }

    if (ext4_validate_group_counts(esi) < 0) {
        printf("[EXT4] Invalid per-group free counts or bitmap bounds\n");
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    esi->block_alloc_hints = (uint32_t *)kcalloc(groups, sizeof(uint32_t));
    esi->inode_alloc_hints = (uint32_t *)kcalloc(groups, sizeof(uint32_t));
    if (!esi->block_alloc_hints || !esi->inode_alloc_hints) {
        printf("[EXT4] Failed to allocate allocator hints\n");
        kfree(esi->block_alloc_hints);
        kfree(esi->inode_alloc_hints);
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    printf("[EXT4] Mounted: block_size=%u groups=%u inode_size=%u inodes/group=%u\n",
           block_size, groups, esi->inode_size, sb.s_inodes_per_group);

    ext4_inode_t root_inode;
    if (ext4_read_inode(esi, EXT4_ROOT_INO, &root_inode) < 0) {
        printf("[EXT4] Failed to read root inode\n");
        kfree(esi->block_alloc_hints);
        kfree(esi->inode_alloc_hints);
        kfree(esi->group_descs);
        kfree(esi);
        return NULL;
    }

    vnode_t *root = ext4_make_vnode(esi, EXT4_ROOT_INO,
                                     ext4_inode_size(&root_inode), VFS_FT_DIR, NULL);
    if (!root) {
        kfree(esi->block_alloc_hints);
        kfree(esi->inode_alloc_hints);
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

    /* Drop all cache-owned vnode references for this filesystem; survivors
     * (still-open files) stay alive on their remaining references and
     * unlinked inodes are reclaimed by release(). */
    vnode_t *held[EXT4_VCACHE_MAX];
    int held_count = 0;
    mutex_lock(&esi->metadata_lock);
    uint64_t cache_flags = spin_lock_irqsave(&g_ext4_vcache_lock);
    for (int i = 0; i < EXT4_VCACHE_MAX; i++) {
        if (g_ext4_vcache[i].vn && g_ext4_vcache[i].sb == esi) {
            held[held_count++] = g_ext4_vcache[i].vn;
            g_ext4_vcache[i].sb = NULL;
            g_ext4_vcache[i].ino = 0;
            g_ext4_vcache[i].vn = NULL;
        }
    }
    spin_unlock_irqrestore(&g_ext4_vcache_lock, cache_flags);
    mutex_unlock(&esi->metadata_lock);
    for (int i = 0; i < held_count; i++)
        vnode_put(held[i]);

    if (root->ops && root->ops->release) root->ops->release(root);
    if (esi->group_descs) {
        kfree(esi->group_descs);
        esi->group_descs = NULL;
    }
    kfree(esi->block_alloc_hints);
    kfree(esi->inode_alloc_hints);
    esi->block_alloc_hints = NULL;
    esi->inode_alloc_hints = NULL;
    kfree(esi);
}

/* ================================================================
 * VFS open hook: create vfile for an ext4 vnode
 * ================================================================ */
