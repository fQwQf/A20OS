#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/gpu/drm_scanout.h"

static unsigned char source[16384], target[16384];
static unsigned calls;
static int sparse;
static int reader(void *opaque, uint64_t offset, void *out, size_t bytes) {
    (void)opaque;
    assert(offset <= sizeof(source) && bytes <= sizeof(source) - offset);
    calls++;
    /* Model noncontiguous VMO pages and an unpopulated page. */
    unsigned char *p = out;
    while (bytes) {
        size_t count = 4096 - offset % 4096;
        if (count > bytes) count = bytes;
        if (sparse && offset / 4096 == 1) memset(p, 0, count);
        else memcpy(p, source + offset, count);
        p += count; bytes -= count; offset += count;
    }
    return 0;
}
static void reset(void) {
    memset(source, 0, sizeof(source)); memset(target, 0xa5, sizeof(target));
    calls = 0; sparse = 0;
}
static uint32_t pixel(const unsigned char *p, unsigned bytes) {
    uint32_t v = 0;
    for (unsigned b = 0; b < bytes; b++) v |= (uint32_t)p[b] << (8 * b);
    return v;
}
static void formats(void) {
    const struct {
        unsigned bpp, r, g, b, expected[4];
    } layouts[] = {
        {32, 0xff0000, 0xff00, 0xff, {0xff0000, 0x00ff00, 0x0000ff, 0x804020}},
        {32, 0xff, 0xff00, 0xff0000, {0xff, 0x00ff00, 0xff0000, 0x204080}},
        {16, 0xf800, 0x07e0, 0x1f, {0xf800, 0x07e0, 0x1f, 0x8204}},
        {16, 0x7c00, 0x03e0, 0x1f, {0x7c00, 0x03e0, 0x1f, 0x4104}},
        {24, 0xff0000, 0xff00, 0xff, {0xff0000, 0x00ff00, 0xff, 0x804020}},
        {32, 0x3ff00000, 0x000ffc00, 0x3ff, {0x3ff00000, 0x000ffc00, 0x3ff, 0x20240480}},
    };
    const unsigned char colors[] = {0,0,255,0, 0,255,0,0, 255,0,0,0, 32,64,128,0};
    for (unsigned l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
        reset();
        memcpy(source, colors, sizeof(colors));
        memcpy(source + 64, colors, sizeof(colors));
        unsigned pb = layouts[l].bpp / 8;
        drm_scanout_t d = {6, 4, 48, layouts[l].bpp, layouts[l].r,
                            layouts[l].g, layouts[l].b, 4 * 48, target};
        uint32_t w = 0, h = 0;
        assert(!drm_scanout_blit(&d, 4, 2, 64, 128, 1, 1, reader, NULL, &w, &h));
        assert(w == 4 && h == 2);
        for (unsigned row = 0; row < 4; row++)
            for (unsigned byte = 0; byte < 48; byte++) {
                if (row >= 1 && row < 3 && byte >= pb && byte < 5 * pb) {
                    unsigned i = (byte - pb) / pb, b = (byte - pb) % pb;
                    assert(target[row * 48 + byte] == ((layouts[l].expected[i] >> (8 * b)) & 255));
                } else assert(target[row * 48 + byte] == 0xa5);
            }
        assert(target[4 * 48] == 0xa5);
    }
}
static void chunks_and_pages(void) {
    reset(); sparse = 1;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (i * 37 + 19) & 255;
    /* 1030 pixels crosses both the conversion chunk and VMO page boundary. */
    drm_scanout_t d = {1030, 2, 4200, 32, 0xff, 0xff00, 0xff0000, 8400, target};
    uint32_t w, h;
    assert(!drm_scanout_blit(&d, 1030, 2, 4160, 8320, 0, 0, reader, NULL, &w, &h));
    assert(w == 1030 && h == 2);
    assert(calls == 34);
    for (unsigned row = 0; row < 2; row++)
        for (unsigned col = 0; col < 1030; col++) {
            unsigned off = row * 4160 + col * 4;
            unsigned expected = 0;
            if (off / 4096 != 1)
                expected = (unsigned)source[off + 2] | (unsigned)source[off + 1] << 8 |
                           (unsigned)source[off] << 16;
            assert(pixel(target + row * 4200 + col * 4, 4) == expected);
        }
    for (unsigned i = 4120; i < 4200; i++) assert(target[i] == 0xa5);
}
static void clipping(void) {
    reset(); memset(source, 0x5a, sizeof(source));
    drm_scanout_t d = {4, 4, 24, 32, 0xff0000, 0xff00, 0xff, 96, target};
    uint32_t w, h;
    assert(!drm_scanout_blit(&d, 8, 8, 64, 512, 3, 3, reader, NULL, &w, &h));
    assert(w == 1 && h == 1 && calls == 1);
    for (unsigned i = 0; i < sizeof(target); i++)
        assert(target[i] == (i >= 84 && i < 88 ? 0x5a : 0xa5));
}
static void bounds(void) {
    reset();
    drm_scanout_t valid = {4, 2, 16, 32, 0xff0000, 0xff00, 0xff, 32, target};
    uint32_t w, h;
    for (unsigned kind = 0; kind < 15; kind++) {
        drm_scanout_t d = valid;
        uint32_t sw = 4, sh = 2, pitch = 16, x = 0, y = 0; uint64_t bytes = 32;
        switch (kind) {
        case 0: d.width = 0; break;
        case 1: d.height = 0; break;
        case 2: d.pitch = 15; break;
        case 3: d.bytes = 31; break;
        case 4: d.bpp = 8; break;
        case 5: d.red = 0xf50000; break;
        case 6: d.red = d.green; break;
        case 7: d.blue = 0; break;
        case 8: d.bpp = 16; break;
        case 9: sw = 0xffffffff; break;
        case 10: sh = 0xffffffff; break;
        case 11: pitch = 15; break;
        case 12: bytes = 31; break;
        case 13: x = 4; break;
        case 14: y = 2; break;
        }
        assert(drm_scanout_blit(&d, sw, sh, pitch, bytes, x, y,
                                reader, NULL, &w, &h) == -EINVAL);
        assert(calls == 0);
        for (unsigned i = 0; i < sizeof(target); i++) assert(target[i] == 0xa5);
    }
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: formats(); break;
    case 1: chunks_and_pages(); break;
    case 2: clipping(); break;
    case 3: bounds(); break;
    default: return 1;
    }
    puts("DRM scanout: PASS"); return 0;
}
