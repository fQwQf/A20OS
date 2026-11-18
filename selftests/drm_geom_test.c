/*
 * Host test for the DRM dumb-buffer geometry.
 *
 * The properties asserted here are all ones a revert would silently break, and
 * all of them were wrong before: the row multiply wrapped in 32 bits, nothing
 * bounded the total, and the buffer-extent consistency that drm_present_buffer_at()
 * relies on was never established at all. A test that only checked the happy path
 * would still be green against the old code, so each case below is a specific
 * defect.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "drivers/gpu/drm_geom.h"

static void test_ordinary_geometry(void)
{
    uint32_t pitch = 0;
    uint64_t size = 0;

    /* 1920x1080x32: 7680 bytes per row, already 64-aligned. */
    assert(drm_dumb_layout(1920, 1080, 32, &pitch, &size) == 0);
    assert(pitch == 7680);
    assert(size == 7680ULL * 1080);

    /* 640x480x32: 2560 bytes per row, no padding needed. */
    assert(drm_dumb_layout(640, 480, 32, &pitch, &size) == 0);
    assert(pitch == 2560);
    assert(size == 2560ULL * 480);

    /* A row that is not a multiple of 64 must be padded up to one. */
    assert(drm_dumb_layout(1, 1, 8, &pitch, &size) == 0);
    assert(pitch == 64);
    assert(size == 64);
}

static void test_pitch_is_always_64_aligned(void)
{
    uint32_t pitch = 0;
    uint64_t size = 0;
    /* Every width from 1..257 at a few depths: pitch must stay 64-aligned and
     * must be able to hold width*bpp/8 bytes. */
    const uint32_t bpps[] = { 8, 16, 24, 32 };
    for (unsigned b = 0; b < sizeof(bpps) / sizeof(bpps[0]); b++) {
        for (uint32_t w = 1; w <= 257; w++) {
            assert(drm_dumb_layout(w, 1, bpps[b], &pitch, &size) == 0);
            assert(pitch % 64 == 0);
            assert(pitch >= ((uint64_t)w * bpps[b] + 7) / 8);
            assert(size == pitch);
        }
    }
}

static void test_rejects_zero_dimensions(void)
{
    uint32_t pitch = 0;
    uint64_t size = 0;
    assert(drm_dumb_layout(0, 480, 32, &pitch, &size) == -1);
    assert(drm_dumb_layout(640, 0, 32, &pitch, &size) == -1);
    assert(drm_dumb_layout(640, 480, 0, &pitch, &size) == -1);
}

static void test_row_multiply_does_not_wrap(void)
{
    uint32_t pitch = 0;
    uint64_t size = 0;
    /* The original computation was ((width * bpp + 7) / 8 + 63) & ~63u in
     * uint32_t, so width * bpp truncated. The damage is a silently truncated
     * geometry rather than a mismatched one -- the old pitch and size stayed
     * consistent with each other, which is why this went unnoticed -- but the
     * caller is entitled to a request it cannot represent to be refused instead
     * of quietly answered with a different buffer. */
    assert(drm_dumb_layout(0x10000000u, 1, 256, &pitch, &size) == -1);
    assert(drm_dumb_layout(0x20000000u, 1, 32, &pitch, &size) == -1);
    assert(drm_dumb_layout(0x40000000u, 1, 32, &pitch, &size) == -1);
    /* 0xffffffff * 32 is 2^37; truncated to 32 bits it is 0xffffffe0, which
     * still yields a large nonzero pitch, so this one was answered with a real
     * buffer whose geometry bears no relation to the request. */
    assert(drm_dumb_layout(0xffffffffu, 1, 32, &pitch, &size) == -1);
}

static void test_total_is_bounded(void)
{
    uint32_t pitch = 0;
    uint64_t size = 0;
    /* 8192x8192x32 is 256 MiB exactly: allowed. */
    assert(drm_dumb_layout(8192, 8192, 32, &pitch, &size) == 0);
    assert(size == DRM_MAX_BUFFER_BYTES);

    /* One row past the cap must be refused. This is the case that used to
     * produce a quarter-gigabyte VMO, and then a quarter-gigabyte kmalloc in
     * PRIME export to snapshot it. */
    assert(drm_dumb_layout(8192, 8193, 32, &pitch, &size) == -1);
    assert(drm_dumb_layout(16384, 8192, 32, &pitch, &size) == -1);
    assert(drm_dumb_layout(65536, 65536, 32, &pitch, &size) == -1);
}

static void test_extent_covers_the_declared_geometry(void)
{
    /* drm_present_buffer_at() walks pitch*height bytes out of the VMO. If the
     * allocation were ever smaller than that, the present path would read past
     * the buffer. Assert the invariant directly rather than trusting that the
     * two computations in drm.c agree. */
    uint32_t pitch = 0;
    uint64_t size = 0;
    const uint32_t dims[][2] = {
        { 1, 1 }, { 17, 3 }, { 64, 64 }, { 320, 240 },
        { 800, 600 }, { 1024, 768 }, { 1280, 1024 }, { 1920, 1080 },
    };
    for (unsigned i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        assert(drm_dumb_layout(dims[i][0], dims[i][1], 32, &pitch, &size) == 0);
        assert(size >= (uint64_t)pitch * dims[i][1]);
        assert(pitch >= dims[i][0] * 4u);
    }
}

int main(void)
{
    test_ordinary_geometry();
    test_pitch_is_always_64_aligned();
    test_rejects_zero_dimensions();
    test_row_multiply_does_not_wrap();
    test_total_is_bounded();
    test_extent_covers_the_declared_geometry();
    printf("drm_geom_test: PASS\n");
    return 0;
}