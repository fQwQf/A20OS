/*
 * A20OS — NTFS read/write filesystem
 *
 * Supported:
 *   - Mount (boot sector + $MFT, USA-fixup aware record I/O)
 *   - Directory listing and lookup ($I30 index root + index allocation)
 *   - File read (resident and non-resident $DATA, sparse runs; no compression)
 *   - File write/truncate: resident data, resident->non-resident conversion,
 *     run-list extension through the $Bitmap cluster allocator
 *   - Create / unlink regular files and directories
 *
 * Known limitations (see docs/fs/ntfs.md):
 *   - Compressed / encrypted attributes are rejected on read.
 *   - Index growth appends to the index root or an existing index allocation
 *     block and returns -ENOSPC when neither has room (no B-tree block split).
 */
#include "fs/ntfs.h"
#include "fs/ntfs_internal.h"
#include "fs/ntfs_format.h"

#include "core/consts.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/sync.h"
#include "fs/block_cache.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#define nput32 nf_put32
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

void ntfs_lock(ntfs_sb_t *sb)
{
    if (sb)
        mutex_lock(&sb->lock);
}

void ntfs_unlock(ntfs_sb_t *sb)
{
    if (sb)
        mutex_unlock(&sb->lock);
}


vnode_ops_t g_ntfs_vnode_ops;
vfile_ops_t g_ntfs_fops;

/* ------------------------------------------------------------------ */
/* Byte/VLI helpers come from ntfs_format.h. */
/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

int ntfs_stream_read(ntfs_sb_t *sb, const ntfs_run_t *runs,
                            uint32_t count, uint64_t size, uint64_t off,
                            void *buf, size_t len)
{
    if (off >= size)
        return 0;
    if (len > size - off)
        len = (size_t)(size - off);
    char *dst = (char *)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t vcn = (off + done) / sb->bytes_per_cluster;
        uint64_t within = (off + done) % sb->bytes_per_cluster;
        uint64_t lcn;
        int r = ntfs_map_vcn(runs, count, vcn, &lcn);
        if (r < 0)
            break;
        size_t chunk = sb->bytes_per_cluster - within;
        if (chunk > len - done)
            chunk = len - done;
        if (r == 0) {
            memset(dst + done, 0, chunk);
        } else if (bcache_read_bytes(sb->bc, lcn * sb->bytes_per_cluster + within,
                                     dst + done, chunk) < 0) {
            break;
        }
        done += chunk;
    }
    return (int)done;
}

int ntfs_stream_write(ntfs_sb_t *sb, const ntfs_run_t *runs,
                             uint32_t count, uint64_t off, const void *buf,
                             size_t len)
{
    const char *src = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        uint64_t vcn = (off + done) / sb->bytes_per_cluster;
        uint64_t within = (off + done) % sb->bytes_per_cluster;
        uint64_t lcn;
        int r = ntfs_map_vcn(runs, count, vcn, &lcn);
        if (r < 0 || r == 0)
            return -1;
        size_t chunk = sb->bytes_per_cluster - within;
        if (chunk > len - done)
            chunk = len - done;
        if (bcache_write_bytes(sb->bc, lcn * sb->bytes_per_cluster + within,
                               src + done, chunk) < 0)
            break;
        done += chunk;
    }
    return (int)done;
}

/* ------------------------------------------------------------------ */
/* USA fixup                                                           */
/* ------------------------------------------------------------------ */

void ntfs_unfixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
                         uint16_t usa_count)
{
    if (usa_count < 2 || usa_off == 0)
        return;
    uint16_t usa_val = nget16(buf + usa_off);
    for (uint16_t i = 1; i < usa_count; i++) {
        uint16_t orig = nget16(buf + usa_off + 2 * i);
        uint64_t so = (uint64_t)i * bps;
        if (nget16(buf + so + bps - 2) == usa_val) {
            buf[so + bps - 2] = (uint8_t)orig;
            buf[so + bps - 1] = (uint8_t)(orig >> 8);
        }
    }
}

void ntfs_fixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
                       uint16_t usa_count)
{
    if (usa_count < 2 || usa_off == 0)
        return;
    uint16_t usa_val = nget16(buf + usa_off);
    for (uint16_t i = 1; i < usa_count; i++) {
        uint64_t so = (uint64_t)i * bps;
        nput16(buf + usa_off + 2 * i, nget16(buf + so + bps - 2));
        buf[so + bps - 2] = (uint8_t)usa_val;
        buf[so + bps - 1] = (uint8_t)(usa_val >> 8);
    }
}

/* ------------------------------------------------------------------ */
/* $MFT record I/O                                                     */
/* ------------------------------------------------------------------ */

int ntfs_build_mft_runs(ntfs_sb_t *sb, uint8_t *rec0)
{
    uint16_t first_attr = nget16(rec0 + 0x14);
    uint32_t off = first_attr;
    while (off + 16 <= sb->mft_record_size) {
        uint32_t type = nget32(rec0 + off);
        uint32_t alen = nget32(rec0 + off + 4);
        if (type == NTFS_AT_END)
            break;
        if (alen < 24 || off + alen > sb->mft_record_size)
            break;
        if (type == NTFS_AT_DATA && rec0[off + 9] == 0 && rec0[off + 8] == 1) {
            uint16_t run_off = nget16(rec0 + off + 0x20);
            sb->mft_data_size = nget64(rec0 + off + 0x30);
            uint8_t *runp = rec0 + off + run_off;
            ntfs_run_t tmp[512];
            uint32_t cap = 0;
            uint64_t prev_lcn = 0;
            for (;;) {
                uint8_t hdr = *runp++;
                uint8_t ll = hdr & 0x0F, lc = hdr >> 4;
                if (ll == 0 && lc == 0)
                    break;
                if (ll > 8 || lc > 8 || cap >= 512)
                    return -1;
                uint64_t length = 0;
                for (int i = 0; i < ll; i++)
                    length |= (uint64_t)runp[i] << (8 * i);
                runp += ll;
                int64_t delta = 0;
                for (int i = 0; i < lc; i++)
                    delta |= (int64_t)runp[i] << (8 * i);
                runp += lc;
                if (lc == 0)
                    tmp[cap].lcn = 0;
                else {
                    prev_lcn += delta;
                    tmp[cap].lcn = prev_lcn;
                }
                tmp[cap].length = length;
                cap++;
            }
            sb->mft_runs = kmalloc(cap * sizeof(ntfs_run_t));
            if (!sb->mft_runs)
                return -1;
            memcpy(sb->mft_runs, tmp, cap * sizeof(ntfs_run_t));
            sb->mft_run_count = cap;
            return 0;
        }
        off += alen;
    }
    return -1;
}

int ntfs_read_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec)
{
    memset(rec, 0, sb->mft_record_size);
    uint64_t byte_off = index * sb->mft_record_size;
    uint64_t clusters = (sb->mft_record_size + sb->bytes_per_cluster - 1) /
                        sb->bytes_per_cluster;
    uint64_t vcn_base = byte_off / sb->bytes_per_cluster;
    for (uint64_t i = 0; i < clusters; i++) {
        uint64_t lcn;
        int r = ntfs_map_vcn(sb->mft_runs, sb->mft_run_count, vcn_base + i, &lcn);
        if (r < 0)
            return -1;
        if (r == 0)
            continue;           /* sparse MFT cluster stays zeroed */
        size_t chunk = sb->mft_record_size - i * sb->bytes_per_cluster;
        if (chunk > sb->bytes_per_cluster)
            chunk = sb->bytes_per_cluster;
        if (bcache_read_bytes(sb->bc, lcn * sb->bytes_per_cluster,
                              rec + i * sb->bytes_per_cluster, chunk) < 0)
            return -1;
    }
    if (memcmp(rec, "FILE", 4) != 0)
        return -1;
    ntfs_unfixup(rec, sb->bytes_per_sector, nget16(rec + 0x04), nget16(rec + 0x06));
    return 0;
}

int ntfs_write_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec)
{
    uint8_t *fix = kmalloc(sb->mft_record_size);
    if (!fix)
        return -1;
    memcpy(fix, rec, sb->mft_record_size);
    ntfs_fixup(fix, sb->bytes_per_sector, nget16(fix + 0x04), nget16(fix + 0x06));

    uint64_t byte_off = index * sb->mft_record_size;
    uint64_t clusters = (sb->mft_record_size + sb->bytes_per_cluster - 1) /
                        sb->bytes_per_cluster;
    uint64_t vcn_base = byte_off / sb->bytes_per_cluster;
    int result = 0;
    for (uint64_t i = 0; i < clusters; i++) {
        uint64_t lcn;
        int r = ntfs_map_vcn(sb->mft_runs, sb->mft_run_count, vcn_base + i, &lcn);
        if (r <= 0) {
            result = -1;
            break;
        }
        size_t chunk = sb->mft_record_size - i * sb->bytes_per_cluster;
        if (chunk > sb->bytes_per_cluster)
            chunk = sb->bytes_per_cluster;
        if (bcache_write_bytes(sb->bc, lcn * sb->bytes_per_cluster,
                               fix + i * sb->bytes_per_cluster, chunk) < 0) {
            result = -1;
            break;
        }
    }
    kfree(fix);
    return result;
}

/* ------------------------------------------------------------------ */
/* Attribute helpers                                                   */
/* ------------------------------------------------------------------ */

uint8_t *ntfs_find_attr(uint8_t *rec, uint64_t rec_size, uint32_t type,
                               int unnamed)
{
    uint16_t first_attr = nget16(rec + 0x14);
    uint32_t off = first_attr;
    while (off + 16 <= rec_size) {
        uint32_t t = nget32(rec + off);
        uint32_t alen = nget32(rec + off + 4);
        if (t == NTFS_AT_END)
            break;
        if (alen < 24 || off + alen > rec_size)
            break;
        if (t == type && (!unnamed || rec[off + 9] == 0))
            return rec + off;
        off += alen;
    }
    return NULL;
}

int ntfs_parse_runs(uint8_t *attr, uint32_t cap, ntfs_run_t *out,
                           uint32_t *out_count, uint64_t *data_size)
{
    uint16_t run_off = nget16(attr + 0x20);
    *data_size = nget64(attr + 0x30);
    uint8_t *runp = attr + run_off;
    uint32_t c = 0;
    uint64_t prev_lcn = 0;
    for (;;) {
        uint8_t hdr = *runp++;
        uint8_t ll = hdr & 0x0F, lc = hdr >> 4;
        if (ll == 0 && lc == 0)
            break;
        if (ll > 8 || lc > 8 || c >= cap)
            return -1;
        uint64_t length = 0;
        for (int i = 0; i < ll; i++)
            length |= (uint64_t)runp[i] << (8 * i);
        runp += ll;
        int64_t delta = 0;
        for (int i = 0; i < lc; i++)
            delta |= (int64_t)runp[i] << (8 * i);
        runp += lc;
        if (lc == 0)
            out[c].lcn = 0;
        else {
            prev_lcn += delta;
            out[c].lcn = prev_lcn;
        }
        out[c].length = length;
        c++;
    }
    *out_count = c;
    return 0;
}

/* ------------------------------------------------------------------ */
/* $DATA resolution                                                    */
/* ------------------------------------------------------------------ */

int ntfs_resolve_data(ntfs_vnode_priv_t *fp)
{
    uint8_t *rec = kmalloc(fp->sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(fp->sb, fp->mft_index, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *data = ntfs_find_attr(rec, fp->sb->mft_record_size, NTFS_AT_DATA, 1);
    int result = -1;
    if (data) {
        if (data[8] == 0) {
            fp->data_resident = 1;
            fp->data_res_off = nget16(data + 0x14);
            fp->data_res_len = nget32(data + 0x10);
            fp->data_size = fp->data_res_len;
            result = 0;
        } else {
            uint16_t flags = nget16(data + 0x0C);
            if (flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED)) {
                result = -1;
            } else {
                fp->data_resident = 0;
                if (!fp->runs) {
                    fp->runs = kmalloc(512 * sizeof(ntfs_run_t));
                    if (!fp->runs) {
                        kfree(rec);
                        return -1;
                    }
                }
                if (ntfs_parse_runs(data, 512, fp->runs, &fp->run_count,
                                    &fp->data_size) == 0)
                    result = 0;
            }
        }
    }
    kfree(rec);
    return result;
}

/* ------------------------------------------------------------------ */
/* Run-list encoding lives in ntfs_format.h. */

/* ------------------------------------------------------------------ */
/* $Bitmap cluster allocator                                           */
/* ------------------------------------------------------------------ */

/* Copy the $Bitmap file's $DATA into *out (freshly allocated). */
int ntfs_load_bmap(ntfs_sb_t *sb, uint8_t **out, uint64_t *out_size)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, NTFS_MFT_REC_BITMAP, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
    int result = -1;
    if (attr) {
        if (attr[8] == 0) {
            uint32_t vlen = nget32(attr + 0x10);
            uint16_t voff = nget16(attr + 0x14);
            uint8_t *data = kmalloc(vlen);
            if (data) {
                memcpy(data, rec + voff, vlen);
                *out = data;
                *out_size = vlen;
                result = 0;
            }
        } else {
            ntfs_run_t runs[1024];
            uint32_t count;
            uint64_t size;
            if (ntfs_parse_runs(attr, 1024, runs, &count, &size) == 0) {
                uint8_t *data = kmalloc(size);
                if (data) {
                    memset(data, 0, size);
                    if (ntfs_stream_read(sb, runs, count, size, 0, data, size) >= 0) {
                        *out = data;
                        *out_size = size;
                        result = 0;
                    } else {
                        kfree(data);
                    }
                }
            }
        }
    }
    kfree(rec);
    return result;
}

/* Write *data back into the $Bitmap file's $DATA. */
int ntfs_save_bmap(ntfs_sb_t *sb, uint8_t *data, uint64_t size)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    if (ntfs_read_record(sb, NTFS_MFT_REC_BITMAP, rec) < 0) {
        kfree(rec);
        return -1;
    }
    uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
    int result = -1;
    if (attr) {
        if (attr[8] == 0) {
            uint32_t vlen = nget32(attr + 0x10);
            uint16_t voff = nget16(attr + 0x14);
            if (vlen >= size) {
                memcpy(rec + voff, data, size);
                result = ntfs_write_record(sb, NTFS_MFT_REC_BITMAP, rec);
            }
        } else {
            ntfs_run_t runs[1024];
            uint32_t count;
            uint64_t size_;
            if (ntfs_parse_runs(attr, 1024, runs, &count, &size_) == 0)
                result = ntfs_stream_write(sb, runs, count, 0, data, size);
        }
    }
    kfree(rec);
    return result;
}

uint64_t ntfs_alloc_clusters(ntfs_sb_t *sb, uint64_t count)
{
    uint8_t *bm = NULL;
    uint64_t bm_size = 0;
    if (ntfs_load_bmap(sb, &bm, &bm_size) < 0 || bm_size == 0) {
        kfree(bm);
        return 0;
    }
    uint64_t total_bits = bm_size * 8;
    if (total_bits > sb->total_clusters)
        total_bits = sb->total_clusters;
    uint64_t found = 0;
    for (uint64_t base = 0; base + count <= total_bits; ) {
        uint64_t run = 0;
        while (base + run < total_bits) {
            if (bm[(base + run) >> 3] & (1u << ((base + run) & 7)))
                break;
            if (++run >= count)
                break;
        }
        if (run >= count) {
            found = base;
            for (uint64_t i = 0; i < count; i++)
                bm[(base + i) >> 3] |= (uint8_t)(1u << ((base + i) & 7));
            if (ntfs_save_bmap(sb, bm, bm_size) < 0)
                found = 0;
            kfree(bm);
            return found;
        }
        base += run ? run : 1;
    }
    kfree(bm);
    return 0;
}

void ntfs_free_clusters(ntfs_sb_t *sb, uint64_t lcn, uint64_t count)
{
    uint8_t *bm = NULL;
    uint64_t bm_size = 0;
    if (ntfs_load_bmap(sb, &bm, &bm_size) < 0)
        return;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t c = lcn + i;
        if (c >= bm_size * 8)
            break;
        bm[c >> 3] &= (uint8_t)~(1u << (c & 7));
    }
    ntfs_save_bmap(sb, bm, bm_size);
    kfree(bm);
}

/* ------------------------------------------------------------------ */
/* MFT record allocation / freeing                                     */
/* ------------------------------------------------------------------ */

/* Find a free MFT record index by scanning (first-fit from @start). */
int64_t ntfs_find_free_record(ntfs_sb_t *sb, uint64_t start)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -1;
    uint64_t max_records = sb->mft_data_size / sb->mft_record_size;
    for (uint64_t i = start; i < max_records; i++) {
        if (ntfs_read_record(sb, i, rec) < 0)
            continue;
        if (!(nget16(rec + 0x16) & NTFS_REC_IN_USE)) {
            kfree(rec);
            return (int64_t)i;
        }
    }
    kfree(rec);
    return -1;
}

void ntfs_free_mft_record(ntfs_sb_t *sb, uint64_t index)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return;
    if (ntfs_read_record(sb, index, rec) == 0) {
        /* Mark free: clear in-use and set signature to "BAAD" is not needed;
         * clearing the in-use flag suffices for our first-fit scan. */
        nput16(rec + 0x16, nget16(rec + 0x16) & ~(uint16_t)NTFS_REC_IN_USE);
        nput32(rec + 0x18, 0);   /* used size */
        ntfs_write_record(sb, index, rec);
    }
    kfree(rec);
}

/* ------------------------------------------------------------------ */
/* $FILE_NAME attribute building                                       */
/* ------------------------------------------------------------------ */

/* Build a full resident $FILE_NAME attribute (header + value) into out.
 * Returns total length. */

/* ------------------------------------------------------------------ */
/* Index (directory) support                                           */
/* ------------------------------------------------------------------ */

/* Iterate the entries of an in-memory index node. */

/* Extract name/is_dir/ref/size from an index entry.  Returns 1 on success. */


/* Load all directory entries into a freshly allocated array. */

/* ------------------------------------------------------------------ */
/* Index entry insertion / removal                                     */
/* ------------------------------------------------------------------ */

/* Build an index entry (without sub-node) into buf. */

/* Insert a new entry into a directory's index (root first, then blocks).
 * Returns 0 on success. */

/* Remove an entry by name from a directory's index (root + blocks). */

/* ------------------------------------------------------------------ */
/* $DATA manipulation (write path)                                     */
/* ------------------------------------------------------------------ */

/* Ensure the file's $DATA covers [off, off+len).  Grows non-resident runs
 * and writes the updated MFT record back.  Called with fp->runs resolved. */

/* Convert a resident $DATA attribute into a non-resident one and allocate
 * clusters for the initial data. */

/* ------------------------------------------------------------------ */
/* vnode lifecycle                                                     */
/* ------------------------------------------------------------------ */




/* ------------------------------------------------------------------ */
/* vnode ops: lookup / stat / create / mkdir / unlink / rmdir          */
/* ------------------------------------------------------------------ */




/* Update the $FILE_NAME size fields in a file's record after size changes. */

/* Rewrite a record's resident $FILE_NAME attribute with a new parent
 * reference and name (used by rename).  The attribute may change length, so
 * the attributes that follow are shifted and the record's used-size is
 * updated.  On-disk state is only touched once the new layout is fully
 * staged in memory. */

/* Create a new MFT record for a file/dir and register it in dir's index. */





/* vnode_ops: rename (file or directory)
 * Steps, all under sb->lock so no other mutation can interleave:
 *   1. resolve the source child and look for a target of the same name;
 *   2. remove the old directory entry;
 *   3. rewrite the child's $FILE_NAME attribute (new parent ref + new name);
 *   4. insert the child into the new directory index.
 * If step 3 or 4 fails, the old entry is restored so the name is never
 * silently lost.  Directory moves repoint the cached vnode's parent. */
/* ------------------------------------------------------------------ */




/* ------------------------------------------------------------------ */
/* vfile ops                                                           */
/* ------------------------------------------------------------------ */







/* ------------------------------------------------------------------ */
/* Common write helper (used by fwrite and writepage)                  */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* vnode / vfile op tables                                             */
/* ------------------------------------------------------------------ */

vnode_ops_t g_ntfs_vnode_ops = {
    .lookup   = ntfs_lookup,
    .create   = ntfs_vn_create,
    .mkdir    = ntfs_vn_mkdir,
    .unlink   = ntfs_vn_unlink,
    .rmdir    = ntfs_vn_rmdir,
    .rename   = ntfs_vn_rename,
    .stat     = ntfs_stat,
    .statfs   = ntfs_statfs,
    .truncate = ntfs_vn_truncate,
    .readpage = ntfs_vn_readpage,
    .writepage = ntfs_vn_writepage,
    .open     = ntfs_open_vnode,
    .release  = ntfs_release_vn,
};

vfile_ops_t g_ntfs_fops = {
    .read    = ntfs_fread,
    .write   = ntfs_fwrite,
    .lseek   = ntfs_flseek,
    .readdir = ntfs_freaddir,
    .close   = ntfs_fclose,
};

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                     */
/* ------------------------------------------------------------------ */

vnode_t *ntfs_mount(struct bcache *bc)
{
    if (!bc)
        return NULL;

    ntfs_sb_t *sb = (ntfs_sb_t *)kmalloc(sizeof(ntfs_sb_t));
    if (!sb)
        return NULL;
    memset(sb, 0, sizeof(*sb));
    mutex_init(&sb->lock);
    sb->bc = bc;

    /* Boot sector. */
    uint8_t boot[512];
    if (bcache_read_bytes(bc, 0, boot, 512) < 0) {
        kfree(sb);
        return NULL;
    }
    if (memcmp(boot + 3, "NTFS    ", 8) != 0) {
        kfree(sb);
        return NULL;
    }
    sb->bytes_per_sector = nget16(boot + 0x0B);
    sb->sectors_per_cluster = boot[0x0D];
    sb->total_clusters = nget64(boot + 0x28) / sb->sectors_per_cluster;
    sb->mft_lcn = nget64(boot + 0x30);
    if (sb->bytes_per_sector == 0 || sb->sectors_per_cluster == 0 ||
        sb->mft_lcn == 0) {
        kfree(sb);
        return NULL;
    }
    sb->bytes_per_cluster = (uint64_t)sb->bytes_per_sector *
                            sb->sectors_per_cluster;

    int8_t cpf = (int8_t)boot[0x40];
    if (cpf > 0)
        sb->mft_record_size = (uint64_t)cpf * sb->bytes_per_cluster;
    else
        sb->mft_record_size = 1ULL << (-cpf);
    int8_t cpi = (int8_t)boot[0x44];
    if (cpi > 0)
        sb->index_record_size = (uint64_t)cpi * sb->bytes_per_cluster;
    else
        sb->index_record_size = 1ULL << (-cpi);
    if (sb->mft_record_size < 1024 || sb->mft_record_size > 4096 ||
        sb->index_record_size < 512) {
        kfree(sb);
        return NULL;
    }

    /* Read MFT record 0 directly (it starts at mft_lcn). */
    uint8_t *rec0 = kmalloc(sb->mft_record_size);
    if (!rec0) {
        kfree(sb);
        return NULL;
    }
    memset(rec0, 0, sb->mft_record_size);
    uint64_t rec0_clusters = (sb->mft_record_size + sb->bytes_per_cluster - 1) /
                             sb->bytes_per_cluster;
    for (uint64_t i = 0; i < rec0_clusters; i++) {
        size_t chunk = sb->mft_record_size - i * sb->bytes_per_cluster;
        if (chunk > sb->bytes_per_cluster)
            chunk = sb->bytes_per_cluster;
        if (bcache_read_bytes(bc, (sb->mft_lcn + i) * sb->bytes_per_cluster,
                              rec0 + i * sb->bytes_per_cluster, chunk) < 0) {
            kfree(rec0);
            kfree(sb);
            return NULL;
        }
    }
    if (memcmp(rec0, "FILE", 4) != 0) {
        kfree(rec0);
        kfree(sb);
        return NULL;
    }
    ntfs_unfixup(rec0, sb->bytes_per_sector, nget16(rec0 + 0x04),
                 nget16(rec0 + 0x06));

    if (ntfs_build_mft_runs(sb, rec0) < 0) {
        kfree(rec0);
        kfree(sb);
        return NULL;
    }
    kfree(rec0);

    /* Root directory (record 5). */
    uint8_t *rec_root = kmalloc(sb->mft_record_size);
    if (!rec_root) {
        kfree(sb->mft_runs);
        kfree(sb);
        return NULL;
    }
    if (ntfs_read_record(sb, NTFS_MFT_REC_ROOT, rec_root) < 0) {
        kfree(rec_root);
        kfree(sb->mft_runs);
        kfree(sb);
        return NULL;
    }
    uint32_t root_flags = nget16(rec_root + 0x16);
    uint32_t root_seq = nget16(rec_root + 0x10);
    kfree(rec_root);

    vnode_t *root = ntfs_make_vnode(sb, NTFS_MFT_REC_ROOT, root_seq,
                                    (root_flags & NTFS_REC_IS_DIR) != 0, NULL);
    if (!root) {
        kfree(sb->mft_runs);
        kfree(sb);
        return NULL;
    }
    root->parent = root;

    kdebug("[NTFS] Mounted: bps=%u spc=%u cluster=%lluB rec=%lluB idx=%lluB\n",
           sb->bytes_per_sector, sb->sectors_per_cluster,
           (unsigned long long)sb->bytes_per_cluster,
           (unsigned long long)sb->mft_record_size,
           (unsigned long long)sb->index_record_size);
    return root;
}

void ntfs_unmount(vnode_t *root)
{
    if (!root || !root->fs_data)
        return;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)root->fs_data;
    ntfs_sb_t *sb = fp->sb;
    bcache_sync(sb->bc);
    if (sb->mft_runs)
        kfree(sb->mft_runs);
    kfree(sb);
    root->fs_data = NULL;
}
