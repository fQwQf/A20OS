#include "boot/framebuffer.h"
#include "core/arch.h"
#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);

/* Firmware RAM scanout keeps its existing WB alias. Never return that alias
 * from the MMIO API, and never install a conflicting Device user alias. */
static void *ram_alias(const boot_framebuffer_t *fb) {
#ifdef CONFIG_NOMMU
    (void)fb;
    return NULL;
#else
    if (!fb->aperture_bytes || fb->physical < PHYS_MEMORY_BASE ||
        fb->physical >= PHYS_MEMORY_END ||
        fb->aperture_bytes > PHYS_MEMORY_END - fb->physical ||
        fb->physical >= 0x100000000ULL ||
        fb->aperture_bytes > 0x100000000ULL - fb->physical)
        return NULL;
    uint64_t mair;
    __asm__ __volatile__("mrs %0, mair_el1" : "=r"(mair));
    if (((mair >> 8) & 255) != 255)
        return NULL;
    uint64_t cursor = fb->physical, end = cursor + fb->aperture_bytes;
    while (cursor < end) {
        uintptr_t va = (uintptr_t)(cursor + PAGE_OFFSET);
        uint64_t *table = boot_pgdir, descriptor = 0, span = 0;
        for (unsigned level = 0; level < 4; level++) {
            unsigned shift = 39 - level * 9;
            descriptor = table[(va >> shift) & 511];
            if (!(descriptor & 1))
                return NULL;
            if (level == 3 || !(descriptor & 2)) {
                if (!level || (descriptor & 0x1c) != 4 ||
                    ((descriptor >> 8) & 3) != 3)
                    return NULL;
                span = 1ULL << shift;
                break;
            }
            table =
                (uint64_t *)(uintptr_t)((descriptor & 0x0000fffffffff000ULL) +
                                        PAGE_OFFSET);
        }
        if (!span || (descriptor & 0x0000fffffffff000ULL & ~(span - 1)) !=
                         (cursor & ~(span - 1)))
            return NULL;
        uint64_t next = (cursor & ~(span - 1)) + span;
        cursor = next < end ? next : end;
    }
    return (void *)(uintptr_t)(fb->physical + PAGE_OFFSET);
#endif
}
void *arch_boot_framebuffer_map(const boot_framebuffer_t *fb, unsigned memory) {
    if (boot_framebuffer_validate(fb))
        return NULL;
    if (memory == BOOT_FRAMEBUFFER_RAM)
        return ram_alias(fb);
    return memory == BOOT_FRAMEBUFFER_DEVICE
               ? arch_device_map_mmio(fb->physical, (size_t)fb->aperture_bytes,
                                      0)
               : NULL;
}
int arch_boot_framebuffer_prepare(const boot_framebuffer_t *fb,
                                  enum boot_framebuffer_memory *memory,
                                  uint64_t dtb, size_t bytes) {
    (void)dtb;
    (void)bytes;
    if (*memory == BOOT_FRAMEBUFFER_FIRMWARE)
        *memory =
            ram_alias(fb) ? BOOT_FRAMEBUFFER_RAM : BOOT_FRAMEBUFFER_DEVICE;
    return arch_boot_framebuffer_map(fb, *memory) ? 0 : -EOPNOTSUPP;
}
int arch_boot_framebuffer_user_flags(const boot_framebuffer_t *fb,
                                     unsigned memory, uintptr_t requested,
                                     uintptr_t *out) {
    if (!arch_boot_framebuffer_map(fb, memory))
        return -EOPNOTSUPP;
    *out = (requested & ~(PTE_DEVICE | PTE_MAT1)) |
           (memory == BOOT_FRAMEBUFFER_RAM ? PTE_MAT1 : PTE_DEVICE);
    return 0;
}
int arch_boot_framebuffer_flush(const boot_framebuffer_t *fb, unsigned memory) {
    void *alias = arch_boot_framebuffer_map(fb, memory);
    if (!alias)
        return -EOPNOTSUPP;
    if (memory == BOOT_FRAMEBUFFER_RAM) {
        uintptr_t first = (uintptr_t)alias;
        size_t line = arch_dcache_line_size();
        uintptr_t end = first + (size_t)fb->stride * fb->height;
        first &= ~((uintptr_t)line - 1);
        for (uintptr_t at = first; at < end; at += line)
            __asm__ __volatile__("dc cvac, %0" ::"r"(at) : "memory");
    }
    /* Complete stores and cache maintenance to PoC before scanout consumes it. */
    __asm__ __volatile__("dsb sy" ::: "memory");
    return 0;
}
