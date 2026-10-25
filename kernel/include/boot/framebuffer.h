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

enum boot_framebuffer_memory {
    BOOT_FRAMEBUFFER_DEVICE = 0,
    BOOT_FRAMEBUFFER_RAM = 1,
    BOOT_FRAMEBUFFER_FIRMWARE = 2
};

/* The descriptor ABI remains firmware-neutral. Memory policy belongs to the
 * copied kernel boot context, never to an unvalidated firmware pointer. */
int arch_boot_framebuffer_prepare(const boot_framebuffer_t *, enum boot_framebuffer_memory *memory,
                                  uint64_t dtb_physical, size_t dtb_bytes);
void *arch_boot_framebuffer_map(const boot_framebuffer_t *, unsigned memory);
int arch_boot_framebuffer_flush(const boot_framebuffer_t *, unsigned memory);
int arch_boot_framebuffer_user_flags(const boot_framebuffer_t *, unsigned memory,
                                     uintptr_t requested, uintptr_t *out);
void *boot_framebuffer_map(void);
int boot_framebuffer_flush(void);
int boot_framebuffer_user_flags(uint64_t physical, size_t bytes,
                                uintptr_t requested, uintptr_t *out);

int boot_framebuffer_validate(const boot_framebuffer_t *fb);
/* Discover the boot scanout as a normal platform device before probe_all. */
int boot_framebuffer_enumerate(void);
#endif
