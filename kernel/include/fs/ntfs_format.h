#ifndef _FS_NTFS_FORMAT_H
#define _FS_NTFS_FORMAT_H

/*
 * Pure NTFS on-disk format helpers shared between the kernel filesystem
 * driver (fs/diskfs/ntfs.c) and the host-side format regression tests
 * (tools/tests/ntfs_format_test.c).  No kernel services are used here.
 */

#include "core/types.h"

/* Little-endian byte access. */
static inline uint16_t nf_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t nf_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t nf_get64(const uint8_t *p) {
    return (uint64_t)nf_get32(p) | ((uint64_t)nf_get32(p + 4) << 32);
}
static inline void nf_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void nf_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void nf_put64(uint8_t *p, uint64_t v) { nf_put32(p, (uint32_t)v); nf_put32(p + 4, (uint32_t)(v >> 32)); }

/* A run-list entry: LCN 0 means a sparse run. */
typedef struct ntfs_run {
    uint64_t lcn;
    uint64_t length;            /* clusters */
} ntfs_run_t;

/* Map a VCN within a run list to an LCN; returns 1 if mapped, 0 if sparse
 * (lcn stays 0), -1 if beyond the mapped range. */
static inline int ntfs_map_vcn(const ntfs_run_t *runs, uint32_t count,
                               uint64_t vcn, uint64_t *lcn)
{
    uint64_t base = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (vcn >= base && vcn < base + runs[i].length) {
            if (runs[i].lcn == 0)
                return 0;
            *lcn = runs[i].lcn + (vcn - base);
            return 1;
        }
        base += runs[i].length;
    }
    return -1;
}

/* Number of bytes needed to encode a VLI. */
static inline int nf_vli_len(uint64_t v)
{
    int n = 1;
    while (v >> (8 * n))
        n++;
    return n;
}

/*
 * Bytes needed to encode a signed run-list LCN delta.  The field must be
 * large enough that the top bit of the most significant byte is not
 * ambiguous: a positive value must not set the sign bit, and a negative
 * value must fit with sign extension (e.g. +128 needs two bytes, -128 only
 * one).
 */
static inline int nf_vli_len_s(int64_t v)
{
    int n = 1;
    if (v >= 0) {
        while (n < 8 && v >= (1LL << (8 * n - 1)))
            n++;
    } else {
        while (n < 8 && v < -(1LL << (8 * n - 1)))
            n++;
    }
    return n;
}

static inline void nf_encode_vli(uint8_t *p, uint64_t v, int len)
{
    for (int i = 0; i < len; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/*
 * Encode a run list (LCNs relative to the previous run) into out.
 * Returns the encoded length, or -1 if it does not fit.
 */
static inline int ntfs_encode_runs(uint8_t *out, size_t cap,
                                   const ntfs_run_t *runs, uint32_t count)
{
    size_t used = 0;
    int64_t prev_lcn = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t length = runs[i].length;
        uint8_t lcn_len;
        int64_t delta = 0;
        if (runs[i].lcn == 0) {
            lcn_len = 0;
        } else {
            delta = (int64_t)runs[i].lcn - prev_lcn;
            prev_lcn = (int64_t)runs[i].lcn;
            lcn_len = (uint8_t)nf_vli_len_s(delta);
        }
        uint8_t ll = (uint8_t)nf_vli_len(length);
        if (used + 1 + ll + lcn_len > cap)
            return -1;
        out[used++] = (uint8_t)(ll | (lcn_len << 4));
        nf_encode_vli(out + used, length, ll);
        used += ll;
        if (lcn_len) {
            nf_encode_vli(out + used, (uint64_t)delta, lcn_len);
            used += lcn_len;
        }
    }
    if (used + 1 > cap)
        return -1;
    out[used++] = 0;
    return (int)used;
}

#endif /* _FS_NTFS_FORMAT_H */
