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

/* Byte/VLI helpers live in ntfs_format.h; keep the local names. */
#define nget16 nf_get16
#define nget32 nf_get32
#define nget64 nf_get64
#define nput16 nf_put16
#define nput32 nf_put32
#define nput64 nf_put64
#define nvli_len nf_vli_len
#define nvli_len_s nf_vli_len_s
#define nenc_vli nf_encode_vli

/* ------------------------------------------------------------------ */
/* On-disk constants                                                   */
/* ------------------------------------------------------------------ */

#define NTFS_AT_STD_INFO     0x10
#define NTFS_AT_ATTR_LIST    0x20
#define NTFS_AT_FILE_NAME    0x30
#define NTFS_AT_DATA         0x80
#define NTFS_AT_INDEX_ROOT   0x90
#define NTFS_AT_INDEX_ALLOC  0xA0
#define NTFS_AT_BITMAP       0xB0
#define NTFS_AT_END          0xFFFFFFFF

#define NTFS_REC_IN_USE 0x0001
#define NTFS_REC_IS_DIR 0x0002

#define NTFS_IDX_ENTRY_NODE 0x01
#define NTFS_IDX_ENTRY_LAST 0x02

#define NTFS_ATTR_COMPRESSED 0x0001
#define NTFS_ATTR_ENCRYPTED  0x4000

#define NTFS_MFT_REC_BITMAP 6
#define NTFS_MFT_REC_ROOT   5

#define NTFS_FILE_NAME_POSIX 1
#define NTFS_ATTR_HEADER_RES 24

/* ------------------------------------------------------------------ */
/* In-memory structures                                                */
/* ------------------------------------------------------------------ */

typedef struct ntfs_sb {
    struct bcache *bc;
    mutex_t lock;

    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint64_t bytes_per_cluster;
    uint64_t total_clusters;
    uint64_t mft_lcn;
    uint64_t mft_record_size;
    uint64_t index_record_size;

    ntfs_run_t *mft_runs;       /* $MFT unnamed $DATA run list */
    uint32_t    mft_run_count;
    uint64_t    mft_data_size;
} ntfs_sb_t;

typedef struct ntfs_vnode_priv {
    ntfs_sb_t *sb;
    uint64_t   mft_index;
    uint32_t   seq;
    int        is_dir;
    int        unlinked;
    /* Cached $DATA location (regular files only). */
    int        data_resident;
    uint32_t   data_res_off;    /* offset within MFT record */
    uint32_t   data_res_len;
    ntfs_run_t *runs;
    uint32_t   run_count;
    uint64_t   data_size;
} ntfs_vnode_priv_t;

typedef struct ntfs_dir_entry {
    char     name[256];
    int      is_dir;
    uint64_t ref;
    uint64_t size;
} ntfs_dir_entry_t;

typedef struct ntfs_fctx {
    ntfs_sb_t *sb;
    uint64_t   mft_index;
    int        is_dir;
    size_t     file_off;
    /* Directory entry cache for readdir. */
    ntfs_dir_entry_t *dirents;
    uint32_t   dirent_count;
    uint32_t   dirent_off;
} ntfs_fctx_t;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void ntfs_lock(ntfs_sb_t *sb)
{
    if (sb)
        mutex_lock(&sb->lock);
}

static void ntfs_unlock(ntfs_sb_t *sb)
{
    if (sb)
        mutex_unlock(&sb->lock);
}

static int ntfs_write_file(ntfs_vnode_priv_t *fp, uint64_t off,
                           const void *buf, size_t len);

static vnode_ops_t g_ntfs_vnode_ops;
static vfile_ops_t g_ntfs_fops;

/* ------------------------------------------------------------------ */
/* Byte/VLI helpers come from ntfs_format.h. */
/* ------------------------------------------------------------------ */
/* Low-level I/O                                                       */
/* ------------------------------------------------------------------ */

static int ntfs_stream_read(ntfs_sb_t *sb, const ntfs_run_t *runs,
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

static int ntfs_stream_write(ntfs_sb_t *sb, const ntfs_run_t *runs,
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

static void ntfs_unfixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
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

static void ntfs_fixup(uint8_t *buf, uint16_t bps, uint16_t usa_off,
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

static int ntfs_build_mft_runs(ntfs_sb_t *sb, uint8_t *rec0)
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

static int ntfs_read_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec)
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

static int ntfs_write_record(ntfs_sb_t *sb, uint64_t index, uint8_t *rec)
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

static uint8_t *ntfs_find_attr(uint8_t *rec, uint64_t rec_size, uint32_t type,
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

static int ntfs_parse_runs(uint8_t *attr, uint32_t cap, ntfs_run_t *out,
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

static int ntfs_resolve_data(ntfs_vnode_priv_t *fp)
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
static int ntfs_load_bmap(ntfs_sb_t *sb, uint8_t **out, uint64_t *out_size)
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
static int ntfs_save_bmap(ntfs_sb_t *sb, uint8_t *data, uint64_t size)
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

static uint64_t ntfs_alloc_clusters(ntfs_sb_t *sb, uint64_t count)
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

static void ntfs_free_clusters(ntfs_sb_t *sb, uint64_t lcn, uint64_t count)
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
static int64_t ntfs_find_free_record(ntfs_sb_t *sb, uint64_t start)
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

static void ntfs_free_mft_record(ntfs_sb_t *sb, uint64_t index)
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
static int ntfs_build_file_name_attr(uint8_t *out, size_t cap,
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

/* ------------------------------------------------------------------ */
/* Index (directory) support                                           */
/* ------------------------------------------------------------------ */

/* Iterate the entries of an in-memory index node. */
typedef int (*ntfs_entry_visit)(const uint8_t *entry, uint16_t entry_len,
                                uint16_t flags, void *ctx);

static void ntfs_walk_node(const uint8_t *node, uint32_t node_size,
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

/* Extract name/is_dir/ref/size from an index entry.  Returns 1 on success. */
static int ntfs_entry_info(const uint8_t *e, uint16_t elen, char *name,
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

typedef struct ntfs_collect_ctx {
    ntfs_sb_t *sb;
    ntfs_dir_entry_t *entries;
    uint32_t max;
    uint32_t *count;
} ntfs_collect_ctx_t;

static int ntfs_collect_entry(const uint8_t *e, uint16_t elen, uint16_t flags,
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

/* Load all directory entries into a freshly allocated array. */
static int ntfs_read_directory(ntfs_vnode_priv_t *fp, ntfs_dir_entry_t **out,
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

/* ------------------------------------------------------------------ */
/* Index entry insertion / removal                                     */
/* ------------------------------------------------------------------ */

/* Build an index entry (without sub-node) into buf. */
static int ntfs_build_index_entry(uint8_t *buf, size_t cap, uint64_t ref,
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

/* Insert a new entry into a directory's index (root first, then blocks).
 * Returns 0 on success. */
static int ntfs_index_insert(ntfs_vnode_priv_t *fp, const char *name,
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

/* Remove an entry by name from a directory's index (root + blocks). */
static int ntfs_index_remove(ntfs_vnode_priv_t *fp, const char *name)
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

/* ------------------------------------------------------------------ */
/* $DATA manipulation (write path)                                     */
/* ------------------------------------------------------------------ */

/* Ensure the file's $DATA covers [off, off+len).  Grows non-resident runs
 * and writes the updated MFT record back.  Called with fp->runs resolved. */
static int ntfs_ensure_data_runs(ntfs_vnode_priv_t *fp, uint64_t need_end)
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

/* Convert a resident $DATA attribute into a non-resident one and allocate
 * clusters for the initial data. */
static int ntfs_convert_resident_to_nonresident(ntfs_vnode_priv_t *fp)
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

/* ------------------------------------------------------------------ */
/* vnode lifecycle                                                     */
/* ------------------------------------------------------------------ */

static int ntfs_lookup(vnode_t *dir, const char *name, vnode_t **out);

static vnode_t *ntfs_make_vnode(ntfs_sb_t *sb, uint64_t mft_index,
                                uint32_t seq, int is_dir, vnode_t *parent)
{
    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn)
        return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino = mft_index;
    vn->type = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0755);
    vn->uid = 0;
    vn->gid = 0;
    vnode_ref_init(vn, 1);
    vn->parent = parent;
    if (parent)
        vnode_get(parent);

    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)kmalloc(sizeof(ntfs_vnode_priv_t));
    if (!fp) {
        kfree(vn);
        return NULL;
    }
    memset(fp, 0, sizeof(*fp));
    fp->sb = sb;
    fp->mft_index = mft_index;
    fp->seq = seq;
    fp->is_dir = is_dir;
    vn->fs_data = fp;
    vn->ops = &g_ntfs_vnode_ops;
    return vn;
}

static void ntfs_release_vn(vnode_t *vn)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return;
    if (fp->runs)
        kfree(fp->runs);
    kfree(fp);
    vn->fs_data = NULL;
}

/* ------------------------------------------------------------------ */
/* vnode ops: lookup / stat / create / mkdir / unlink / rmdir          */
/* ------------------------------------------------------------------ */

static int ntfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    if (!out || !dir)
        return -EINVAL;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp || !fp->is_dir)
        return -ENOTDIR;

    ntfs_dir_entry_t *entries = NULL;
    uint32_t count = 0;
    if (ntfs_read_directory(fp, &entries, &count) < 0)
        return -EIO;
    vnode_t *found = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            uint64_t ref = entries[i].ref;
            uint64_t mft = ref & 0x0000FFFFFFFFFFFFULL;
            uint32_t seq = (uint32_t)((ref >> 48) & 0xFFFF);
            found = ntfs_make_vnode(fp->sb, mft, seq, entries[i].is_dir, dir);
            if (found)
                found->size = entries[i].size;
            break;
        }
    }
    kfree(entries);
    if (!found)
        return -ENOENT;
    *out = found;
    return 0;
}

static int ntfs_stat(vnode_t *vn, kstat_t *st)
{
    if (!vn || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_size = (int64_t)vn->size;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_blocks = 0;
    st->st_blksize = 512;
    st->st_nlink = 1;
    return 0;
}

static int ntfs_statfs(vnode_t *vn, kstatfs_t *st)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->f_bsize = fp->sb->bytes_per_cluster;
    st->f_blocks = fp->sb->total_clusters;
    st->f_bfree = fp->sb->total_clusters / 2;
    st->f_bavail = fp->sb->total_clusters / 2;
    st->f_namelen = 255;
    return 0;
}

/* Update the $FILE_NAME size fields in a file's record after size changes. */
static void ntfs_update_file_name_size(ntfs_sb_t *sb, uint64_t mft_index,
                                       uint64_t size, int is_dir)
{
    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return;
    if (ntfs_read_record(sb, mft_index, rec) < 0) {
        kfree(rec);
        return;
    }
    uint8_t *fn = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_FILE_NAME, 0);
    if (fn && fn[8] == 0) {
        uint16_t voff = nget16(fn + 0x14);
        uint8_t *v = rec + voff;
        nput64(v + 0x28, size);
        nput64(v + 0x30, size);
        if (is_dir)
            nput32(v + 0x38, NTFS_REC_IS_DIR);
        ntfs_write_record(sb, mft_index, rec);
    }
    kfree(rec);
}

/* Create a new MFT record for a file/dir and register it in dir's index. */
static int ntfs_create_entry(ntfs_vnode_priv_t *dirfp, const char *name,
                             int mode, int is_dir, vnode_t **out)
{
    (void)mode;
    ntfs_sb_t *sb = dirfp->sb;
    int64_t free_idx = ntfs_find_free_record(sb, 12);
    if (free_idx < 0)
        return -ENOSPC;

    uint8_t *rec = kmalloc(sb->mft_record_size);
    if (!rec)
        return -ENOMEM;
    memset(rec, 0, sb->mft_record_size);

    /* Build a fresh FILE record. */
    memcpy(rec, "FILE", 4);
    uint16_t usa_count = (uint16_t)(sb->mft_record_size / sb->bytes_per_sector + 1);
    nput16(rec + 0x04, 0x30);
    nput16(rec + 0x06, usa_count);
    nput16(rec + 0x10, 1);              /* sequence number */
    nput16(rec + 0x12, 1);              /* hard link count */
    nput16(rec + 0x14, 0x30);           /* first attribute offset */
    nput16(rec + 0x16, NTFS_REC_IN_USE | (is_dir ? NTFS_REC_IS_DIR : 0));
    nput32(rec + 0x1C, sb->mft_record_size);
    nput16(rec + 0x28, 3);              /* next attribute id */
    nput32(rec + 0x2C, (uint32_t)free_idx);
    /* USA value placeholder (0xFFFF); fixup fills real values. */
    nput16(rec + 0x30, 0xFFFF);

    uint32_t off = 0x30;

    /* $STANDARD_INFORMATION (resident, 72-byte value). */
    {
        uint32_t alen = NTFS_ATTR_HEADER_RES + 72;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_STD_INFO);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 1);
        nput32(rec + off + 0x10, 72);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        off += alen;
    }

    /* $FILE_NAME. */
    {
        uint8_t fn[1024];
        int fn_len = ntfs_build_file_name_attr(fn, sizeof(fn),
                                               dirfp->mft_index |
                                               ((uint64_t)dirfp->seq << 48),
                                               name, is_dir, 0);
        if (fn_len < 0) {
            kfree(rec);
            return -EINVAL;
        }
        memcpy(rec + off, fn, fn_len);
        off += fn_len;
    }

    if (is_dir) {
        /* $INDEX_ROOT for $I30. */
        uint32_t vlen = 16 + 16 + 16;   /* root + node + terminator */
        uint32_t alen = NTFS_ATTR_HEADER_RES + vlen;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_INDEX_ROOT);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 3);
        nput32(rec + off + 0x10, vlen);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        uint8_t *v = rec + off + NTFS_ATTR_HEADER_RES;
        nput32(v + 0x00, NTFS_AT_FILE_NAME);   /* index type */
        nput32(v + 0x04, 1);                   /* collation: file name */
        nput32(v + 0x08, (uint32_t)sb->index_record_size);
        v[0x0C] = (uint8_t)(sb->index_record_size / sb->bytes_per_cluster);
        uint8_t *node = v + 16;
        nput32(node + 0x00, 16);               /* entries offset */
        nput32(node + 0x04, 16);               /* total size */
        nput32(node + 0x08, 16);               /* allocated size */
        nput32(node + 0x0C, 0);                /* flags */
        /* 16-byte terminator entry. */
        nput64(node + 16, 0);
        nput16(node + 16 + 2, 16);
        nput16(node + 16 + 8, NTFS_IDX_ENTRY_LAST);
        off += alen;
    } else {
        /* Empty resident $DATA. */
        uint32_t alen = NTFS_ATTR_HEADER_RES;
        memset(rec + off, 0, alen);
        nput32(rec + off + 0x00, NTFS_AT_DATA);
        nput32(rec + off + 0x04, alen);
        rec[off + 8] = 0;
        rec[off + 9] = 0;
        nput16(rec + off + 0x0A, NTFS_ATTR_HEADER_RES);
        nput16(rec + off + 0x0E, 2);
        nput32(rec + off + 0x10, 0);
        nput16(rec + off + 0x14, NTFS_ATTR_HEADER_RES);
        off += alen;
    }

    /* End marker. */
    nput32(rec + off, NTFS_AT_END);
    nput32(rec + 0x18, off + 4);

    if (ntfs_write_record(sb, (uint64_t)free_idx, rec) < 0) {
        kfree(rec);
        return -EIO;
    }
    kfree(rec);

    /* Insert into the parent directory index. */
    int r = ntfs_index_insert(dirfp, name, (uint64_t)free_idx |
                              ((uint64_t)1 << 48), is_dir, 0);
    if (r < 0) {
        ntfs_free_mft_record(sb, (uint64_t)free_idx);
        return r;
    }

    vnode_t *vn = ntfs_make_vnode(sb, (uint64_t)free_idx, 1, is_dir, NULL);
    if (vn)
        vn->size = 0;
    if (out)
        *out = vn;
    return 0;
}

static int ntfs_vn_create(vnode_t *dir, const char *name, int mode, vnode_t **out)
{
    (void)mode;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);
    int r = ntfs_create_entry(fp, name, mode, 0, out);
    ntfs_unlock(fp->sb);
    return r;
}

static int ntfs_vn_mkdir(vnode_t *dir, const char *name, int mode)
{
    (void)mode;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);
    int r = ntfs_create_entry(fp, name, mode, 1, NULL);
    ntfs_unlock(fp->sb);
    return r;
}

static int ntfs_vn_unlink(vnode_t *dir, const char *name)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);

    vnode_t *victim = NULL;
    int lr = ntfs_lookup(dir, name, &victim);
    if (lr < 0) {
        ntfs_unlock(fp->sb);
        return lr;
    }
    ntfs_vnode_priv_t *vp = (ntfs_vnode_priv_t *)victim->fs_data;
    if (vp->is_dir) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -EISDIR;
    }

    int r = ntfs_index_remove(fp, name);
    if (r == 0) {
        /* Free data clusters and the MFT record. */
        if (ntfs_resolve_data(vp) == 0 && !vp->data_resident && vp->runs) {
            for (uint32_t i = 0; i < vp->run_count; i++)
                ntfs_free_clusters(fp->sb, vp->runs[i].lcn, vp->runs[i].length);
        }
        ntfs_free_mft_record(fp->sb, vp->mft_index);
    }
    vnode_put(victim);
    ntfs_unlock(fp->sb);
    return r;
}

static int ntfs_vn_rmdir(vnode_t *dir, const char *name)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)dir->fs_data;
    if (!fp)
        return -EINVAL;
    ntfs_lock(fp->sb);

    vnode_t *victim = NULL;
    int lr = ntfs_lookup(dir, name, &victim);
    if (lr < 0) {
        ntfs_unlock(fp->sb);
        return lr;
    }
    ntfs_vnode_priv_t *vp = (ntfs_vnode_priv_t *)victim->fs_data;
    if (!vp->is_dir) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -ENOTDIR;
    }

    ntfs_dir_entry_t *entries = NULL;
    uint32_t count = 0;
    int empty = ntfs_read_directory(vp, &entries, &count) == 0 && count == 0;
    kfree(entries);
    if (!empty) {
        vnode_put(victim);
        ntfs_unlock(fp->sb);
        return -ENOTEMPTY;
    }

    int r = ntfs_index_remove(fp, name);
    if (r == 0)
        ntfs_free_mft_record(fp->sb, vp->mft_index);
    vnode_put(victim);
    ntfs_unlock(fp->sb);
    return r;
}

/* ------------------------------------------------------------------ */
/* vnode ops: readpage / writepage                                     */
/* ------------------------------------------------------------------ */

static int ntfs_vn_readpage(vnode_t *vn, uint64_t index, void *data, size_t len)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return -EINVAL;
    uint64_t off = index * PAGE_SIZE;
    memset(data, 0, len);
    ntfs_lock(fp->sb);
    int r = 0;
    if (ntfs_resolve_data(fp) < 0) {
        r = -EIO;
    } else if (fp->data_resident) {
        if (off < fp->data_size) {
            size_t n = fp->data_size - off;
            if (n > len)
                n = len;
            uint8_t *rec = kmalloc(fp->sb->mft_record_size);
            if (rec) {
                if (ntfs_read_record(fp->sb, fp->mft_index, rec) == 0)
                    memcpy(data, rec + fp->data_res_off + off, n);
                else
                    r = -EIO;
                kfree(rec);
            } else {
                r = -ENOMEM;
            }
        }
    } else {
        if (off < fp->data_size) {
            size_t n = fp->data_size - off;
            if (n > len)
                n = len;
            if (ntfs_stream_read(fp->sb, fp->runs, fp->run_count,
                                 fp->data_size, off, data, n) < 0)
                r = -EIO;
        }
    }
    ntfs_unlock(fp->sb);
    return r;
}

static int ntfs_vn_writepage(vnode_t *vn, uint64_t index, const void *data, size_t len)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return -EINVAL;
    uint64_t off = index * PAGE_SIZE;
    ntfs_lock(fp->sb);
    int r = ntfs_write_file(fp, off, data, len);
    ntfs_unlock(fp->sb);
    return r;
}

static int ntfs_vn_truncate(vnode_t *vn, size_t size)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp || fp->is_dir)
        return -EINVAL;
    ntfs_lock(fp->sb);
    if (ntfs_resolve_data(fp) < 0) {
        ntfs_unlock(fp->sb);
        return -EIO;
    }
    /* Shrink the initialized size; for non-resident data we cannot easily
     * shrink the allocation, so we just drop the size. */
    fp->data_size = size;
    vn->size = size;

    uint8_t *rec = kmalloc(fp->sb->mft_record_size);
    int r = -1;
    if (rec) {
        if (ntfs_read_record(fp->sb, fp->mft_index, rec) == 0) {
            uint8_t *attr = ntfs_find_attr(rec, fp->sb->mft_record_size,
                                           NTFS_AT_DATA, 1);
            if (attr) {
                if (attr[8] == 0) {
                    nput32(attr + 0x10, (uint32_t)size);
                    ntfs_update_file_name_size(fp->sb, fp->mft_index, size, 0);
                } else {
                    nput64(attr + 0x30, size);
                    nput64(attr + 0x38, size);
                    ntfs_update_file_name_size(fp->sb, fp->mft_index, size, 0);
                }
                r = ntfs_write_record(fp->sb, fp->mft_index, rec);
            }
        }
        kfree(rec);
    }
    ntfs_unlock(fp->sb);
    return r;
}

/* ------------------------------------------------------------------ */
/* vfile ops                                                           */
/* ------------------------------------------------------------------ */

static vfile_t *ntfs_open_vnode(vnode_t *vn, int flags)
{
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return NULL;

    ntfs_fctx_t *fc = (ntfs_fctx_t *)kmalloc(sizeof(ntfs_fctx_t));
    vfile_t *vf = vfile_alloc();
    if (!fc || !vf) {
        if (fc) kfree(fc);
        if (vf) vfile_free(vf);
        return NULL;
    }
    memset(fc, 0, sizeof(*fc));
    fc->sb = fp->sb;
    fc->mft_index = fp->mft_index;
    fc->is_dir = fp->is_dir;

    if (fp->is_dir) {
        ntfs_lock(fp->sb);
        if (ntfs_read_directory(fp, &fc->dirents, &fc->dirent_count) < 0)
            fc->dirent_count = 0;
        ntfs_unlock(fp->sb);
    }

    vfile_ref_init(vf, 1);
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vf->ops = &g_ntfs_fops;
    vf->priv = fc;
    return vf;
}

static int ntfs_fread(vfile_t *vf, char *buf, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp || fc->is_dir)
        return -EISDIR;
    ntfs_lock(fc->sb);
    if (ntfs_resolve_data(fp) < 0) {
        ntfs_unlock(fc->sb);
        return -EIO;
    }
    size_t fsize = fp->data_size;
    if (fc->file_off >= fsize) {
        ntfs_unlock(fc->sb);
        return 0;
    }
    size_t n = fsize - fc->file_off;
    if (n > count)
        n = count;
    int r;
    if (fp->data_resident) {
        uint8_t *rec = kmalloc(fp->sb->mft_record_size);
        if (!rec) { ntfs_unlock(fc->sb); return -ENOMEM; }
        if (ntfs_read_record(fp->sb, fp->mft_index, rec) < 0) {
            kfree(rec); ntfs_unlock(fc->sb); return -EIO;
        }
        memcpy(buf, rec + fp->data_res_off + fc->file_off, n);
        kfree(rec);
        r = (int)n;
    } else {
        r = ntfs_stream_read(fp->sb, fp->runs, fp->run_count, fp->data_size,
                             fc->file_off, buf, n);
    }
    fc->file_off += (size_t)r;
    vf->offset = fc->file_off;
    ntfs_unlock(fc->sb);
    return r;
}

static int ntfs_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp || fc->is_dir)
        return -EISDIR;
    ntfs_lock(fc->sb);
    int r = ntfs_write_file(fp, fc->file_off, buf, count);
    if (r > 0) {
        fc->file_off += (size_t)r;
        vf->offset = fc->file_off;
        if (vf->vnode->size < fc->file_off)
            vf->vnode->size = fc->file_off;
    }
    ntfs_unlock(fc->sb);
    return r;
}

static long ntfs_flseek(vfile_t *vf, long offset, int whence)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    ntfs_vnode_priv_t *fp = (ntfs_vnode_priv_t *)vf->vnode->fs_data;
    if (!fc || !fp)
        return -EBADF;
    size_t base = 0;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = fc->file_off;
    else if (whence == SEEK_END)
        base = fp->data_size;
    else
        return -EINVAL;
    fc->file_off = (size_t)(offset < 0 ? 0 : (long)(base) + (offset < 0 ? 0 : (unsigned long)offset));
    vf->offset = fc->file_off;
    return (long)fc->file_off;
}

static int ntfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    if (!fc || !fc->is_dir)
        return -ENOTDIR;

    char *out = (char *)dirp;
    size_t total = 0;
    while (fc->dirent_off < fc->dirent_count) {
        ntfs_dir_entry_t *d = &fc->dirents[fc->dirent_off];
        size_t namelen = strlen(d->name);
        size_t reclen = sizeof(vfs_dirent64_t) + namelen + 1;
        reclen = (reclen + 7) & ~(size_t)7;
        if (total + reclen > count)
            break;
        vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
        dent->d_ino = d->ref & 0x0000FFFFFFFFFFFFULL;
        dent->d_off = (int64_t)fc->dirent_off;
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type = d->is_dir ? 4 : 8;   /* DT_DIR / DT_REG */
        memcpy(dent->d_name, d->name, namelen + 1);
        total += reclen;
        fc->dirent_off++;
    }
    return (int)total;
}

static int ntfs_fclose(vfile_t *vf)
{
    ntfs_fctx_t *fc = (ntfs_fctx_t *)vf->priv;
    if (fc) {
        if (fc->dirents)
            kfree(fc->dirents);
        kfree(fc);
        vf->priv = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Common write helper (used by fwrite and writepage)                  */
/* ------------------------------------------------------------------ */

static int ntfs_write_file(ntfs_vnode_priv_t *fp, uint64_t off,
                           const void *buf, size_t len)
{
    ntfs_sb_t *sb = fp->sb;
    if (fp->is_dir)
        return -EISDIR;
    if (len == 0)
        return 0;
    if (ntfs_resolve_data(fp) < 0)
        return -EIO;

    uint64_t end = off + len;
    uint64_t resident_limit = sb->mft_record_size - 0x60;

    /* Try to stay resident. */
    if (fp->data_resident && end <= resident_limit) {
        uint8_t *rec = kmalloc(sb->mft_record_size);
        if (!rec)
            return -ENOMEM;
        if (ntfs_read_record(sb, fp->mft_index, rec) < 0) {
            kfree(rec);
            return -EIO;
        }
        uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size, NTFS_AT_DATA, 1);
        int r = -1;
        if (attr && attr[8] == 0) {
            uint16_t voff = nget16(attr + 0x14);
            if (off < fp->data_size)
                memcpy(rec + voff + off, buf, len);
            else {
                if (off > fp->data_size)
                    memset(rec + voff + fp->data_size, 0, (size_t)(off - fp->data_size));
                memcpy(rec + voff + off, buf, len);
            }
            nput32(attr + 0x10, (uint32_t)end);
            uint32_t used = nget32(rec + 0x18);
            if (used < voff + end)
                nput32(rec + 0x18, voff + (uint32_t)end);
            fp->data_size = end;
            fp->data_res_len = (uint32_t)end;
            ntfs_update_file_name_size(sb, fp->mft_index, end, 0);
            r = ntfs_write_record(sb, fp->mft_index, rec);
        }
        kfree(rec);
        if (r < 0)
            return r;
        return (int)len;
    }

    /* Convert to non-resident if needed. */
    if (fp->data_resident) {
        if (ntfs_convert_resident_to_nonresident(fp) < 0)
            return -EIO;
    }

    /* Ensure runs cover the target range. */
    if (ntfs_ensure_data_runs(fp, end) < 0)
        return -ENOSPC;

    int r = ntfs_stream_write(sb, fp->runs, fp->run_count, off, buf, len);
    if (r <= 0)
        return r;
    if (end > fp->data_size) {
        fp->data_size = end;
        ntfs_update_file_name_size(sb, fp->mft_index, end, 0);
        /* Update $DATA initialized size. */
        uint8_t *rec = kmalloc(sb->mft_record_size);
        if (rec) {
            if (ntfs_read_record(sb, fp->mft_index, rec) == 0) {
                uint8_t *attr = ntfs_find_attr(rec, sb->mft_record_size,
                                               NTFS_AT_DATA, 1);
                if (attr && attr[8] == 1) {
                    nput64(attr + 0x38, end);
                    nput64(attr + 0x30, end);
                    ntfs_write_record(sb, fp->mft_index, rec);
                }
            }
            kfree(rec);
        }
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* vnode / vfile op tables                                             */
/* ------------------------------------------------------------------ */

static vnode_ops_t g_ntfs_vnode_ops = {
    .lookup   = ntfs_lookup,
    .create   = ntfs_vn_create,
    .mkdir    = ntfs_vn_mkdir,
    .unlink   = ntfs_vn_unlink,
    .rmdir    = ntfs_vn_rmdir,
    .rename   = NULL,
    .stat     = ntfs_stat,
    .statfs   = ntfs_statfs,
    .truncate = ntfs_vn_truncate,
    .readpage = ntfs_vn_readpage,
    .writepage = ntfs_vn_writepage,
    .open     = ntfs_open_vnode,
    .release  = ntfs_release_vn,
};

static vfile_ops_t g_ntfs_fops = {
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
