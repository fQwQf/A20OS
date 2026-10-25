#include "boot/context.h"
#include "core/arch.h"
#include "core/errno.h"

extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t)
    __attribute__((weak));
__attribute__((weak)) int
arch_boot_framebuffer_prepare(const boot_framebuffer_t *fb,
                              enum boot_framebuffer_memory *memory,
                              uint64_t dtb, size_t bytes) {
    (void)fb;
    (void)dtb;
    (void)bytes;
    if (*memory == BOOT_FRAMEBUFFER_FIRMWARE && arch_device_map_mmio &&
        arch_device_map_mmio(fb->physical, (size_t)fb->aperture_bytes, 0))
        *memory = BOOT_FRAMEBUFFER_DEVICE;
    return *memory == BOOT_FRAMEBUFFER_DEVICE ? 0 : -EOPNOTSUPP;
}
__attribute__((weak)) void *
arch_boot_framebuffer_map(const boot_framebuffer_t *fb, unsigned memory) {
    if (memory != BOOT_FRAMEBUFFER_DEVICE || !arch_device_map_mmio)
        return NULL;
    return arch_device_map_mmio(fb->physical, (size_t)fb->aperture_bytes, 0);
}
__attribute__((weak)) int
arch_boot_framebuffer_flush(const boot_framebuffer_t *fb, unsigned memory) {
    (void)fb;
    if (memory != BOOT_FRAMEBUFFER_DEVICE)
        return -EOPNOTSUPP;
    arch_mb();
    return 0;
}
__attribute__((weak)) int
arch_boot_framebuffer_user_flags(const boot_framebuffer_t *fb, unsigned memory,
                                 uintptr_t requested, uintptr_t *out) {
    (void)fb;
    if (memory != BOOT_FRAMEBUFFER_DEVICE)
        return -EOPNOTSUPP;
    *out = requested | PTE_DEVICE;
    return 0;
}
void *boot_framebuffer_map(void) {
    const boot_context_t *boot = boot_context_get();
    return arch_boot_framebuffer_map(&boot->framebuffer,
                                     boot->framebuffer_memory);
}
int boot_framebuffer_flush(void) {
    const boot_context_t *boot = boot_context_get();
    return arch_boot_framebuffer_flush(&boot->framebuffer,
                                       boot->framebuffer_memory);
}
int boot_framebuffer_user_flags(uint64_t physical, size_t bytes,
                                uintptr_t requested, uintptr_t *out) {
    if (!out)
        return -EINVAL;
    const boot_context_t *boot = boot_context_get();
    const boot_framebuffer_t *fb = &boot->framebuffer;
    if (!fb->magic || physical < fb->physical || !bytes ||
        physical - fb->physical >= fb->aperture_bytes ||
        bytes > fb->aperture_bytes - (physical - fb->physical)) {
        *out = requested | PTE_DEVICE;
        return 0;
    }
    return arch_boot_framebuffer_user_flags(fb, boot->framebuffer_memory,
                                            requested, out);
}
