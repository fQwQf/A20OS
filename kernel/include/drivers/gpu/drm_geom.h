/*
 * Buffer geometry for the DRM dumb-buffer path.
 *
 * Kept apart from drm.c, and free of kernel types, for one reason: the
 * arithmetic here is the part that has to be right about overflow rather than
 * about locking, and a host test can only check what it can compile. Everything
 * below is pure integer maths over fixed-width types, so tools/tests/
 * drm_geom_test.c can drive it directly instead of the logic being asserted by
 * inspection.
 */
#ifndef _DRIVERS_GPU_DRM_GEOM_H
#define _DRIVERS_GPU_DRM_GEOM_H

#include <stdint.h>

/* Ceiling on one buffer object.
 *
 * Two things need a bound and neither had one. CREATE_DUMB's size is
 * width*pitch*height computed from userspace numbers, and PRIME export copies
 * the whole buffer into kernel heap with kmalloc(b->size) -- so without a cap a
 * single ioctl could ask for a multi-gigabyte VMO and then a multi-gigabyte
 * kernel allocation to snapshot it.
 *
 * 256 MiB is not arbitrary: it is exactly the largest resource the 3D transport
 * will publish, because drm_gem_attach_backing() already refuses more than
 * 65536 pages. One limit for both paths means a buffer that can be rendered
 * into is always a buffer that can be exported.
 */
#define DRM_MAX_BUFFER_BYTES (256ULL * 1024 * 1024)

/* Bytes per row, 64-byte aligned, and the total buffer size.
 *
 * Returns 0 on success and -1 when the request cannot be honoured: any zero
 * dimension or zero depth, a row that cannot be represented, a total that
 * overflows 64 bits, or a total above DRM_MAX_BUFFER_BYTES.
 *
 * The row computation is deliberately 64-bit. Done in uint32_t -- which is what
 * it used to be -- width * bpp wraps for large dimensions, and the wrapped pitch
 * then multiplies out to a size far smaller than the geometry claims, so the
 * buffer's advertised extent and its real extent disagree.
 */
static inline int drm_dumb_layout(uint32_t width, uint32_t height, uint32_t bpp,
                                  uint32_t *out_pitch, uint64_t *out_size)
{
    if (!width || !height || !bpp || !out_pitch || !out_size)
        return -1;

    uint64_t bits = (uint64_t)width * (uint64_t)bpp;
    if (bits / bpp != width)               /* row wider than 64 bits */
        return -1;
    uint64_t row = (bits + 7) / 8;
    if (row > 0xffffffffULL)
        return -1;
    uint64_t pitch = (row + 63) & ~63ULL;
    if (pitch == 0 || pitch > 0xffffffffULL)
        return -1;

    uint64_t size = pitch * (uint64_t)height;
    if (size / height != pitch)            /* total overflowed 64 bits */
        return -1;
    if (size == 0 || size > DRM_MAX_BUFFER_BYTES)
        return -1;

    *out_pitch = (uint32_t)pitch;
    *out_size = size;
    return 0;
}

#endif /* _DRIVERS_GPU_DRM_GEOM_H */