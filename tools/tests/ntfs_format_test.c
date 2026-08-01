/*
 * Host-side regression tests for the NTFS on-disk format helpers
 * (kernel/include/fs/ntfs_format.h).  Compiled with the host gcc; no kernel
 * services are used.
 *
 * Build & run:
 *   gcc -I../../kernel/include -O2 -Wall -Wextra tools/tests/ntfs_format_test.c -o /tmp/ntfs_format_test
 *   /tmp/ntfs_format_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "fs/ntfs_format.h"

static void test_byte_helpers(void)
{
    uint8_t b[8] = {0};
    nf_put16(b, 0x1234);
    assert(nf_get16(b) == 0x1234);
    nf_put32(b, 0x12345678);
    assert(nf_get32(b) == 0x12345678);
    nf_put64(b, 0x0123456789abcdefULL);
    assert(nf_get64(b) == 0x0123456789abcdefULL);
    assert(b[0] == 0xef && b[7] == 0x01);
}

static void test_vli_lengths(void)
{
    assert(nf_vli_len(0) == 1);
    assert(nf_vli_len(0xff) == 1);
    assert(nf_vli_len(0x100) == 2);
    assert(nf_vli_len(0xffffffffULL) == 4);
    assert(nf_vli_len(0x100000000ULL) == 5);
    /* Signed run-list deltas must not leave an ambiguous sign bit. */
    assert(nf_vli_len_s(0) == 1);
    assert(nf_vli_len_s(127) == 1);
    assert(nf_vli_len_s(128) == 2);
    assert(nf_vli_len_s(32767) == 2);
    assert(nf_vli_len_s(32768) == 3);
    assert(nf_vli_len_s(-1) == 1);
    assert(nf_vli_len_s(-128) == 1);
    assert(nf_vli_len_s(-129) == 2);
    assert(nf_vli_len_s(-32768) == 2);
    assert(nf_vli_len_s(-32769) == 3);
}

/* Encode a run list and decode it back, verifying round-trip. */
static void test_run_encode_roundtrip(void)
{
    /* Contiguous runs + one sparse run. */
    ntfs_run_t runs[] = {
        { .lcn = 100, .length = 5 },
        { .lcn = 200, .length = 3 },
        { .lcn = 0,   .length = 4 },   /* sparse */
        { .lcn = 300, .length = 2 },
    };
    uint8_t enc[128];
    int len = ntfs_encode_runs(enc, sizeof(enc), runs, 4);
    assert(len > 0);

    /* Manually decode the run list (NTFS format) and compare. */
    const uint8_t *p = enc;
    ntfs_run_t out[8];
    uint32_t count = 0;
    int64_t prev = 0;
    for (;;) {
        uint8_t hdr = *p++;
        uint8_t ll = hdr & 0x0f;
        uint8_t lc = hdr >> 4;
        if (ll == 0 && lc == 0)
            break;
        uint64_t length = 0;
        for (int i = 0; i < ll; i++)
            length |= (uint64_t)p[i] << (8 * i);
        p += ll;
        int64_t delta = 0;
        for (int i = 0; i < lc; i++)
            delta |= (int64_t)p[i] << (8 * i);
        p += lc;
        if (lc == 0)
            out[count].lcn = 0;
        else {
            prev += delta;
            out[count].lcn = prev;
        }
        out[count].length = length;
        count++;
    }
    assert(count == 4);
    for (uint32_t i = 0; i < count; i++) {
        assert(out[i].lcn == runs[i].lcn);
        assert(out[i].length == runs[i].length);
    }

    /* Buffer too small must fail cleanly. */
    assert(ntfs_encode_runs(enc, 3, runs, 4) == -1);
    /* Single zero-length run is just a terminator. */
    ntfs_run_t one[] = { { .lcn = 7, .length = 1 } };
    int l1 = ntfs_encode_runs(enc, sizeof(enc), one, 1);
    assert(l1 == 4);   /* header + 1 len byte + 1 lcn byte + terminator */
    assert(enc[0] == 0x11);
}

static void test_run_map(void)
{
    ntfs_run_t runs[] = {
        { .lcn = 100, .length = 10 },
        { .lcn = 0,   .length = 5 },
        { .lcn = 300, .length = 7 },
    };
    uint64_t lcn = 0;
    assert(ntfs_map_vcn(runs, 3, 0, &lcn) == 1 && lcn == 100);
    assert(ntfs_map_vcn(runs, 3, 9, &lcn) == 1 && lcn == 109);
    assert(ntfs_map_vcn(runs, 3, 10, &lcn) == 0);   /* sparse */
    assert(ntfs_map_vcn(runs, 3, 14, &lcn) == 0);
    assert(ntfs_map_vcn(runs, 3, 15, &lcn) == 1 && lcn == 300);
    assert(ntfs_map_vcn(runs, 3, 21, &lcn) == 1 && lcn == 306);
    assert(ntfs_map_vcn(runs, 3, 22, &lcn) == -1);  /* past end */
}

static void test_offsets(void)
{
    /* On-disk struct layout sanity (packed little-endian layouts). */
    assert(offsetof(ntfs_run_t, lcn) == 0);
    assert(offsetof(ntfs_run_t, length) == 8);
}

int main(void)
{
    test_byte_helpers();
    test_vli_lengths();
    test_run_encode_roundtrip();
    test_run_map();
    test_offsets();
    printf("ntfs_format_test: PASS\n");
    return 0;
}
