#ifndef A20_DRM_SCANOUT_H
#define A20_DRM_SCANOUT_H

#include "core/types.h"
#include "core/errno.h"

/* KMS dumb buffers use XRGB8888 independently of the retained display's
 * wire format. Only the final copy needs to know the hardware pitch/masks.
 * The reader also handles VMO page boundaries and sparse zero-filled pages.
 * Format definitions: Linux v6.17 include/uapi/drm/drm_fourcc.h (XRGB8888).
 * Firmware layout: UEFI GOP EFI_GRAPHICS_OUTPUT_MODE_INFORMATION defines
 * PixelFormat/PixelInformation and PixelsPerScanLine independently of width:
 * https://github.com/tianocore/edk2/blob/edk2-stable202508/MdePkg/Include/Protocol/GraphicsOutput.h
 * https://github.com/torvalds/linux/blob/v6.17/include/uapi/drm/drm_fourcc.h */
typedef struct drm_scanout {
    uint32_t width, height, pitch, bpp;
    uint32_t red, green, blue;
    uint64_t bytes;
    uint8_t *pixels;
} drm_scanout_t;

typedef int (*drm_scanout_read_t)(void *, uint64_t, void *, size_t);

static inline int drm_scanout_mask(uint32_t mask, unsigned *shift, uint32_t *max) {
    if (!mask) return -EINVAL;
    *shift = 0;
    while (!(mask & 1)) { (*shift)++; mask >>= 1; }
    if (mask & (mask + 1)) return -EINVAL;
    *max = mask;
    return 0;
}

static inline int drm_scanout_blit(const drm_scanout_t *dst,
                                    uint32_t sw, uint32_t sh, uint32_t pitch,
                                    uint64_t bytes, uint32_t x, uint32_t y,
                                    drm_scanout_read_t read, void *opaque,
                                    uint32_t *out_width, uint32_t *out_height) {
    if (!dst || !dst->pixels || !read || !out_width || !out_height ||
        !dst->width || !dst->height || !sw || !sh ||
        (dst->bpp != 16 && dst->bpp != 24 && dst->bpp != 32) ||
        (uint64_t)dst->width * (dst->bpp / 8) > dst->pitch ||
        (uint64_t)dst->pitch * dst->height > dst->bytes ||
        dst->bytes > SIZE_MAX || bytes > SIZE_MAX ||
        (uint64_t)sw * 4 > pitch || (uint64_t)pitch * sh > bytes ||
        x >= dst->width || y >= dst->height)
        return -EINVAL;
    uint32_t all = dst->red | dst->green | dst->blue;
    if ((dst->red & dst->green) || (dst->red & dst->blue) ||
        (dst->green & dst->blue) || (dst->bpp < 32 && (all >> dst->bpp)))
        return -EINVAL;
    unsigned shift[3]; uint32_t max[3];
    if (drm_scanout_mask(dst->red, &shift[0], &max[0]) ||
        drm_scanout_mask(dst->green, &shift[1], &max[1]) ||
        drm_scanout_mask(dst->blue, &shift[2], &max[2]))
        return -EINVAL;
    uint32_t width = dst->width - x, height = dst->height - y;
    if (width > sw) width = sw;
    if (height > sh) height = sh;
    unsigned pixel_bytes = dst->bpp / 8;
    int direct = dst->bpp == 32 && dst->red == 0xff0000 &&
                 dst->green == 0xff00 && dst->blue == 0xff;
    for (uint32_t row = 0; row < height; row++) {
        uint64_t source = (uint64_t)row * pitch;
        uint8_t *target = dst->pixels + (size_t)(y + row) * dst->pitch +
                          (size_t)x * pixel_bytes;
        if (direct) {
            int rc = read(opaque, source, target, (size_t)width * 4);
            if (rc) return rc;
            continue;
        }
        /* Bounded stack use, including for 16K-wide firmware modes. Decode
         * byte order explicitly: DRM_FORMAT_XRGB8888 is B,G,R,X in memory. */
        uint8_t input[256];
        for (uint32_t col = 0; col < width;) {
            uint32_t count = width - col;
            if (count > sizeof(input) / 4) count = sizeof(input) / 4;
            int rc = read(opaque, source + (uint64_t)col * 4, input, count * 4);
            if (rc) return rc;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t pixel = 0;
                const unsigned channels[] = {2, 1, 0};
                for (unsigned c = 0; c < 3; c++)
                    pixel |= (uint32_t)(((uint64_t)input[i * 4 + channels[c]] *
                                         max[c] + 127) / 255) << shift[c];
                for (unsigned b = 0; b < pixel_bytes; b++)
                    target[(size_t)(col + i) * pixel_bytes + b] = pixel >> (b * 8);
            }
            col += count;
        }
    }
    *out_width = width; *out_height = height;
    return 0;
}

#endif
