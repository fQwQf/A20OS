#ifndef A20_BOOT_FRAMEBUFFER_H
#define A20_BOOT_FRAMEBUFFER_H
#include "core/types.h"

#define BOOT_FRAMEBUFFER_MAGIC 0x41463230U
#define BOOT_FRAMEBUFFER_VERSION 1U
/* A copied, firmware-independent scanout description. No firmware pointers or
 * virtual addresses survive in this structure. Zero magic means absent. */
typedef struct boot_framebuffer {
    uint32_t magic, version;
    uint64_t physical, aperture_bytes;
    uint32_t width, height, stride, bits_per_pixel;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
} boot_framebuffer_t;

int boot_framebuffer_validate(const boot_framebuffer_t *fb);
/* Discover the boot scanout as a normal platform device before probe_all. */
int boot_framebuffer_enumerate(void);
#endif
