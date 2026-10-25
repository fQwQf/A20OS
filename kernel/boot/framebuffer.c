#include "boot/framebuffer.h"
#include "core/errno.h"

static int contiguous(uint32_t mask) {
    if (!mask)
        return 0;
    while (!(mask & 1U))
        mask >>= 1;
    return !(mask & (mask + 1U));
}
int boot_framebuffer_validate(const boot_framebuffer_t *fb) {
    if (!fb || fb->magic != BOOT_FRAMEBUFFER_MAGIC ||
        fb->version != BOOT_FRAMEBUFFER_VERSION || !fb->physical ||
        (fb->physical & 4095) || !fb->width || !fb->height ||
        fb->width > 16384 || fb->height > 16384 ||
        (fb->bits_per_pixel != 16 && fb->bits_per_pixel != 24 &&
         fb->bits_per_pixel != 32))
        return -EINVAL;
    uint32_t bytes = fb->bits_per_pixel / 8;
    uint64_t visible = (uint64_t)fb->stride * fb->height;
    if (fb->stride < fb->width * bytes || fb->stride % bytes ||
        visible > 256ULL * 1024 * 1024 || !visible ||
        ((visible + 4095) & ~4095ULL) > fb->aperture_bytes ||
        fb->aperture_bytes > (uint64_t)-1 - fb->physical ||
        fb->aperture_bytes > 256ULL * 1024 * 1024)
        return -EOVERFLOW;
    uint32_t r = fb->red_mask, g = fb->green_mask, b = fb->blue_mask;
    uint32_t all = r | g | b | fb->reserved_mask;
    if (!contiguous(r) || !contiguous(g) || !contiguous(b) || (r & g) ||
        (r & b) || (g & b) || ((r | g | b) & fb->reserved_mask) ||
        (fb->bits_per_pixel < 32 && (all >> fb->bits_per_pixel)))
        return -EINVAL;
    return 0;
}
