#include "fs/ntfs.h"
#include "fs/ntfs_format.h"
#include "fs/ntfs_internal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/sync.h"
#include "fs/block_cache.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/* NTFS index: $I30 B-tree directory machinery. */

int ntfs_build_file_name_attr(uint8_t *out, size_t cap,
                                     uint64_t parent_ref, const char *name,
                                     int is_dir, uint64_t data_size)
{
    size_t nlen = strlen(name);
    size_t nbytes = nlen * 2;
    uint32_t value_len = 0x42 + (uint32_t)nbytes;
    uint32_t total = NTFS_ATTR_HEADER_RES + value_len;
    if (total > cap)
        return -1;

    memset(out, 0, total);
    nput32(out + 0x00, NTFS_AT_FILE_NAME);
    nput32(out + 0x04, total);
    out[8] = 0;                 /* resident */
    out[9] = 0;                 /* unnamed */
    nput16(out + 0x0A, NTFS_ATTR_HEADER_RES);
    nput16(out + 0x0E, 0x02);
    nput32(out + 0x10, value_len);
    nput16(out + 0x14, NTFS_ATTR_HEADER_RES);

    uint8_t *v = out + NTFS_ATTR_HEADER_RES;
    nput64(v + 0x00, parent_ref);
    nput64(v + 0x08, 0);        /* creation time (0 = default) */
    nput64(v + 0x10, 0);
    nput64(v + 0x18, 0);
    nput64(v + 0x20, 0);
    nput64(v + 0x28, data_size);
    nput64(v + 0x30, data_size);
    nput32(v + 0x38, is_dir ? NTFS_REC_IS_DIR : 0);
    nput32(v + 0x3C, 0);
    v[0x40] = (uint8_t)nlen;
    v[0x41] = NTFS_FILE_NAME_POSIX;
    for (size_t i = 0; i < nlen; i++) {
        v[0x42 + 2 * i] = (uint8_t)name[i];
        v[0x42 + 2 * i + 1] = 0;
    }
    return (int)total;
}


void ntfs_walk_node(const uint8_t *node, uint32_t node_size,
                           ntfs_entry_visit visit, void *ctx)
{
    if (node_size < 16)
        return;
    uint32_t off = nget32(node + 0);
    uint32_t total = nget32(node + 4);
    if (off == 0 || off >= node_size || total == 0 || total > node_size)
        return;
    while (off + 16 <= total) {
        const uint8_t *e = node + off;
        uint16_t elen = nget16(e + 2);
        uint16_t flags = nget16(e + 8);
        if (elen < 16 || off + elen > total)
            break;
        if (visit(e, elen, flags, ctx) < 0)
            return;
        if (flags & NTFS_IDX_ENTRY_LAST)
            break;
        off += elen;
    }
}


int ntfs_entry_info(const uint8_t *e, uint16_t elen, char *name,
                           size_t name_cap, int *is_dir, uint64_t *ref,
                           uint64_t *size)
{
    const uint8_t *stream = e + 16;
    uint16_t stream_len = nget16(e + 6);
    if (stream_len < NTFS_ATTR_HEADER_RES + 0x42 || stream[0] != NTFS_AT_FILE_NAME)
        return 0;
    /* $FILE_NAME attribute value starts after its resident header. */
    const uint8_t *v = stream + NTFS_ATTR_HEADER_RES;
    uint8_t nlen = v[0x40];
    size_t nbytes = nlen * 2;
    if (NTFS_ATTR_HEADER_RES + 0x42 + nbytes > stream_len)
        return 0;
    size_t out = 0;
    for (int i = 0; i < nlen; i++) {
        uint16_t cu = (uint16_t)(v[0x42 + 2 * i] | (v[0x42 + 2 * i + 1] << 8));
        if (cu > 0x7F)
            cu = '?';
        if (out + 1 < name_cap)
            name[out++] = (char)cu;
    }
    name[out] = '\0';
    *is_dir = (nget32(v + 0x38) & NTFS_REC_IS_DIR) != 0;
    *ref = nget64(e);
    *size = nget64(v + 0x30);
    (void)elen;
    return 1;
}


int ntfs_collect_entry(const uint8_t *e, uint16_t elen, uint16_t flags,
                              void *ctx)
{
    ntfs_collect_ctx_t *c = (ntfs_collect_ctx_t *)ctx;
    (void)elen;
    if ((flags & NTFS_IDX_ENTRY_LAST) || (e[0] == 0 && e[1] == 0 && e[2] == 0))
        return 0;
    if (*c->count >= c->max)
        return 0;
    ntfs_dir_entry_t *d = &c->entries[*c->count];
    if (ntfs_entry_info(e, elen, d->name, sizeof(d->name), &d->is_dir,
                        &d->ref, &d->size)) {
        (*c->count)++;
    }
    return 0;
}


int ntfs_read_directory(ntfs_vnode_priv_t *fp, ntfs_dir_entry_t **out,
                               uint32_t *out_count)
{
    ntfs_sb_t *sb = fp->sb;
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }

    uint8_t *ix_root = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ROOT, 1);
    uint8_t *ix_alloc = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ALLOC, 1);
    if (!ix_root) {
        kfree(rec);
        return -1;
    }

    /* Two passes: count then fill. */
    uint32_t count = 0;
    ntfs_collect_ctx_t cctx;
    cctx.sb = sb;
    cctx.max = 0;
    cctx.count = &count;

    /* Pass 1: walk the index root node. */
    if (ix_root[8] == 0) {
        uint16_t voff = nget16(ix_root + 0x14);
        uint32_t vlen = nget32(ix_root + 0x10);
        const uint8_t *node = rec + voff + 16;   /* after ntfs_index_root_t */
        ntfs_walk_node(node, vlen - 16, ntfs_collect_entry, &cctx);
    }

    /* Pass 1: walk index allocation blocks. */
    ntfs_run_t *blocks = NULL;
    uint32_t block_count = 0;
    uint64_t block_size = 0;
    int have_blocks = 0;
    if (ix_alloc && ix_alloc[8] == 1) {
        uint16_t flags = nget16(ix_alloc + 0x0C);
        if (!(flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED))) {
            blocks = kmalloc(1024 * sizeof(ntfs_run_t));
            if (blocks && ntfs_parse_runs(ix_alloc, 1024, blocks, &block_count,
                                          &block_size) == 0)
                have_blocks = 1;
        }
    }

    if (have_blocks) {
        uint8_t *blk = kmalloc(sb->index_record_size);
        if (blk) {
            uint64_t nblocks = block_size / sb->index_record_size;
            for (uint64_t bi = 0; bi < nblocks; bi++) {
                memset(blk, 0, sb->index_record_size);
                if (ntfs_stream_read(sb, blocks, block_count, block_size,
                                     bi * sb->index_record_size, blk,
                                     sb->index_record_size) < 0)
                    break;
                if (memcmp(blk, "INDX", 4) != 0)
                    break;
                ntfs_unfixup(blk, sb->bytes_per_sector, nget16(blk + 0x04),
                             nget16(blk + 0x06));
                ntfs_walk_node(blk + 24, sb->index_record_size - 24,
                               ntfs_collect_entry, &cctx);
            }
            kfree(blk);
        }
        kfree(blocks);
    }
    kfree(rec);

    if (count == 0)
        return 0;

    ntfs_dir_entry_t *arr = kmalloc(count * sizeof(ntfs_dir_entry_t));
    if (!arr)
        return -1;
    uint32_t real = 0;
    ntfs_collect_ctx_t c2;
    c2.sb = sb;
    c2.entries = arr;
    c2.max = count;
    c2.count = &real;

    /* Re-walk to fill. */
    rec = kmalloc(sb->mft_record_size);
    if (!rec) { kfree(arr); return -1; }
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(arr); kfree(rec); return -1;
    }
    ix_root = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ROOT, 1);
    ix_alloc = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ALLOC, 1);
    if (ix_root && ix_root[8] == 0) {
        uint16_t voff = nget16(ix_root + 0x14);
        uint32_t vlen = nget32(ix_root + 0x10);
        ntfs_walk_node(rec + voff + 16, vlen - 16, ntfs_collect_entry, &c2);
    }
    blocks = NULL;
    block_count = 0;
    block_size = 0;
    have_blocks = 0;
    if (ix_alloc && ix_alloc[8] == 1) {
        blocks = kmalloc(1024 * sizeof(ntfs_run_t));
        if (blocks && ntfs_parse_runs(ix_alloc, 1024, blocks, &block_count,
                                      &block_size) == 0)
            have_blocks = 1;
    }
    if (have_blocks) {
        uint8_t *blk = kmalloc(sb->index_record_size);
        if (blk) {
            uint64_t nblocks = block_size / sb->index_record_size;
            for (uint64_t bi = 0; bi < nblocks; bi++) {
                memset(blk, 0, sb->index_record_size);
                if (ntfs_stream_read(sb, blocks, block_count, block_size,
                                     bi * sb->index_record_size, blk,
                                     sb->index_record_size) < 0)
                    break;
                if (memcmp(blk, "INDX", 4) != 0)
                    break;
                ntfs_unfixup(blk, sb->bytes_per_sector, nget16(blk + 0x04),
                             nget16(blk + 0x06));
                ntfs_walk_node(blk + 24, sb->index_record_size - 24,
                               ntfs_collect_entry, &c2);
            }
            kfree(blk);
        }
        kfree(blocks);
    }
    kfree(rec);

    *out = arr;
    *out_count = real;
    return 0;
}


int ntfs_build_index_entry(uint8_t *buf, size_t cap, uint64_t ref,
                                  const char *name, int is_dir,
                                  uint64_t data_size, int is_last)
{
    uint8_t fn[1024];
    int fn_len = ntfs_build_file_name_attr(fn, sizeof(fn), 0, name, is_dir,
                                           data_size);
    if (fn_len < 0)
        return -1;
    size_t total = 16 + (size_t)fn_len;
    if (is_last)
        total += 8;             /* padding to 8-byte boundary handled below */
    total = (total + 7) & ~(size_t)7;
    if (total > cap || total > 0xFFFF)
        return -1;

    memset(buf, 0, total);
    nput64(buf + 0, ref);
    nput16(buf + 2, (uint16_t)total);
    nput16(buf + 4, 0);         /* no indexed $FILE_NAME (indexing flag) */
    nput16(buf + 6, (uint16_t)fn_len);
    nput16(buf + 8, is_last ? NTFS_IDX_ENTRY_LAST : 0);
    memcpy(buf + 16, fn, fn_len);
    return (int)total;
}


int ntfs_index_insert(ntfs_vnode_priv_t *fp, const char *name,
                             uint64_t ref, int is_dir, uint64_t data_size)
{
    ntfs_sb_t *sb = fp->sb;
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }

    uint8_t *ix_root = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ROOT, 1);
    if (!ix_root || ix_root[8] != 0) {
        kfree(rec);
        return -1;
    }

    /* Build entry sized for a "last" entry (no following entries needed). */
    uint8_t entry[1024];
    int elen = ntfs_build_index_entry(entry, sizeof(entry), ref, name, is_dir,
                                      data_size, 1);
    if (elen < 0) {
        kfree(rec);
        return -1;
    }

    uint16_t voff = nget16(ix_root + 0x14);
    uint32_t vlen = nget32(ix_root + 0x10);
    uint8_t *node = rec + voff + 16;   /* index root node */
    uint32_t node_total = nget32(node + 4);
    uint32_t node_alloc = nget32(node + 8);
    uint32_t entries_off = nget32(node + 0);

    /* Walk to the last entry; its length is where we insert. */
    uint32_t last_off = entries_off;
    uint32_t off = entries_off;
    for (;;) {
        const uint8_t *e = node + off;
        uint16_t f = nget16(e + 8);
        uint16_t l = nget16(e + 2);
        if (f & NTFS_IDX_ENTRY_LAST) {
            last_off = off;
            break;
        }
        off += l;
        if (off + 16 > node_total)
            break;
    }
    /* We replace the last entry's terminator: the last entry usually is just
     * an 8-byte terminator (file_ref=0, entry_len=8, flags=LAST) plus 8 bytes
     * of padding.  We insert our new entry before it. */
    uint32_t need = (uint32_t)elen;
    if (last_off + need + 16 > voff + vlen - 16) {
        /* No room in the index root. */
        kfree(rec);
        return -ENOSPC;
    }

    /* Move the terminator aside and insert the new entry. */
    uint32_t tail_off = last_off;
    uint32_t tail_len = node_total - last_off;
    memmove(node + tail_off + need, node + tail_off, tail_len);
    memcpy(node + tail_off, entry, need);
    nput32(node + 4, node_total + need);
    nput32(node + 8, node_alloc);
    /* Grow the attribute's value length. */
    nput32(ix_root + 0x10, vlen + need);

    /* Update record used size. */
    uint32_t used = nget32(rec + 0x18);
    if (used + need <= sb->mft_record_size) {
        nput32(rec + 0x18, used + need);
    }

    int r = ntfs_write_record(sb, fp->mft_index, rec);
    kfree(rec);
    return r;
}


int ntfs_index_remove(ntfs_vnode_priv_t *fp, const char *name)
{
    ntfs_sb_t *sb = fp->sb;
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *ix_root = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_INDEX_ROOT, 1);
    if (!ix_root || ix_root[8] != 0) {
        kfree(rec);
        return -1;
    }
    uint16_t voff = nget16(ix_root + 0x14);
    uint32_t vlen = nget32(ix_root + 0x10);
    uint8_t *node = rec + voff + 16;
    uint32_t node_total = nget32(node + 4);

    uint32_t off = nget32(node + 0);
    uint32_t prev = 0;
    int removed = 0;
    while (off + 16 <= node_total) {
        uint8_t *e = node + off;
        uint16_t flags = nget16(e + 8);
        uint16_t elen = nget16(e + 2);
        if (elen < 16 || off + elen > node_total)
            break;
        char n[256];
        int is_dir;
        uint64_t ref, size;
        int is_target = ntfs_entry_info(e, elen, n, sizeof(n), &is_dir,
                                        &ref, &size) &&
                        strcmp(n, name) == 0;
        if (is_target && !(flags & NTFS_IDX_ENTRY_LAST)) {
            /* Remove: memmove the rest over this entry. */
            uint32_t rest = node_total - (off + elen);
            memmove(node + off, node + off + elen, rest);
            nput32(node + 4, node_total - elen);
            nput32(ix_root + 0x10, vlen - elen);
            removed = 1;
            break;
        }
        prev = off;
        if (flags & NTFS_IDX_ENTRY_LAST)
            break;
        off += elen;
    }
    (void)prev;
    if (!removed) {
        kfree(rec);
        return -ENOENT;
    }
    uint32_t used = nget32(rec + 0x18);
    nput32(rec + 0x18, used > 16 ? used - 16 : 16);
    int r = ntfs_write_record(sb, fp->mft_index, rec);
    kfree(rec);
    return r;
}


int ntfs_ensure_data_runs(ntfs_vnode_priv_t *fp, uint64_t need_end)
{
    ntfs_sb_t *sb = fp->sb;
    uint64_t alloc_end = 0;
    for (uint32_t i = 0; i < fp->run_count; i++)
        alloc_end += fp->runs[i].length * sb->bytes_per_cluster;
    if (need_end <= alloc_end)
        return 0;

    uint64_t want_clusters = (need_end - alloc_end + sb->bytes_per_cluster - 1) /
                             sb->bytes_per_cluster;
    uint64_t lcn = ntfs_alloc_clusters(sb, want_clusters);
    if (lcn == 0)
        return -ENOSPC;

    if (fp->run_count >= 512)
        return -ENOSPC;
    fp->runs[fp->run_count].lcn = lcn;
    fp->runs[fp->run_count].length = want_clusters;
    fp->run_count++;

    /* Update the MFT record's $DATA attribute. */
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
    int result = -1;
    if (attr && attr[8] == 1) {
        uint8_t runbuf[1024];
        int rlen = ntfs_encode_runs(runbuf, sizeof(runbuf), fp->runs,
                                    fp->run_count);
        if (rlen > 0) {
            uint16_t run_off = nget16(attr + 0x20);
            /* The run list must fit in the attribute; resize attribute if needed. */
            size_t attr_len = nget32(attr + 4);
            size_t header_len = run_off;
            size_t new_attr_len = header_len + (size_t)rlen;
            /* Reallocate attribute by rewriting the whole record is complex;
             * assume the existing attribute slack is enough. */
            if (new_attr_len <= attr_len) {
                memcpy(rec + (uintptr_t)attr - (uintptr_t)rec + run_off,
                       runbuf, rlen);
                nput64(attr + 0x28, alloc_end + want_clusters * sb->bytes_per_cluster);
                nput64(attr + 0x38, need_end);
                nput64(attr + 0x30, need_end);
                nput32(rec + 0x18, nget32(rec + 0x18) + (uint32_t)(attr_len - attr_len));
                result = ntfs_write_record(sb, fp->mft_index, rec);
            }
        }
    }
    kfree(rec);
    return result;
}


int ntfs_convert_resident_to_nonresident(ntfs_vnode_priv_t *fp)
{
    ntfs_sb_t *sb = fp->sb;
    uint32_t res_len = fp->data_res_len;
    uint32_t res_off = fp->data_res_off;
    uint64_t clusters = (res_len + sb->bytes_per_cluster - 1) /
                        sb->bytes_per_cluster;
    if (clusters == 0)
        clusters = 1;
    uint64_t lcn = ntfs_alloc_clusters(sb, clusters);
    if (lcn == 0)
        return -ENOSPC;

    fp->runs = kmalloc(512 * sizeof(ntfs_run_t));
    if (!fp->runs)
        return -ENOMEM;
    fp->runs[0].lcn = lcn;
    fp->runs[0].length = clusters;
    fp->run_count = 1;

    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec) {
        ntfs_free_clusters(sb, lcn, clusters);
        return -ENOMEM;
    }
    if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
    if (!attr) {
        kfree(rec);
        return -1;
    }

    /* Read resident data before we overwrite the attribute. */
    uint8_t *tmp = kmalloc(res_len ? res_len : 1);
    if (!tmp) {
        kfree(rec);
        return -ENOMEM;
    }
    if (res_len)
        memcpy(tmp, rec + res_off, res_len);

    /* Rebuild the attribute as non-resident.  We reuse the same slot and
     * expand it; a 1024-byte run buffer leaves room. */
    uint8_t runbuf[1024];
    int rlen = ntfs_encode_runs(runbuf, sizeof(runbuf), fp->runs, 1);
    if (rlen <= 0) {
        kfree(tmp);
        kfree(rec);
        return -1;
    }

    uint32_t attr_off = (uint32_t)((uintptr_t)attr - (uintptr_t)rec);
    uint32_t old_alen = nget32(rec + attr_off + 4);
    uint32_t new_alen = 0x40 + (uint32_t)rlen;
    if (new_alen > old_alen) {
        /* Grow: move following attributes. */
        uint32_t attr_end = attr_off + old_alen;
        uint32_t rec_end = nget32(rec + 0x18);
        size_t move_len = rec_end - attr_end;
        memmove(rec + attr_off + new_alen, rec + attr_off + old_alen, move_len);
    }
    memset(rec + attr_off, 0, new_alen);
    attr = rec + attr_off;
    nput32(rec + attr_off + 0x00, NTFS_AT_DATA);
    nput32(rec + attr_off + 0x04, new_alen);
    rec[attr_off + 0x08] = 1;   /* non-resident */
    rec[attr_off + 0x09] = 0;
    nput16(rec + attr_off + 0x0A, 0x40);
    nput16(rec + attr_off + 0x0E, nget16(rec + attr_off + 0x0E));
    nput64(rec + attr_off + 0x10, 0);           /* lowest VCN */
    nput64(rec + attr_off + 0x18, clusters - 1); /* highest VCN */
    nput16(rec + attr_off + 0x20, 0x40);        /* run list offset */
    rec[attr_off + 0x22] = 0;                   /* compression unit */
    nput64(rec + attr_off + 0x28, clusters * sb->bytes_per_cluster);
    nput64(rec + attr_off + 0x30, res_len);
    nput64(rec + attr_off + 0x38, res_len);
    memcpy(rec + attr_off + 0x40, runbuf, rlen);

    uint32_t used = nget32(rec + 0x18);
    nput32(rec + 0x18, used + (new_alen > old_alen ? new_alen - old_alen : 0));

    /* Write the resident data into the new clusters. */
    ntfs_stream_write(sb, fp->runs, 1, 0, tmp, res_len);
    kfree(tmp);

    fp->data_resident = 0;
    fp->data_size = res_len;
    int r = ntfs_write_record(sb, fp->mft_index, rec);
    kfree(rec);
    return r;
}
