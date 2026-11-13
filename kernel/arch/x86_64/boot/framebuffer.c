#include "boot/framebuffer.h"
#include "core/string.h"
#include "platform.h"

extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
extern uint64_t x86_boot_framebuffer_pa;
int x86_boot_framebuffer_get(boot_framebuffer_t *out) {
    memset(out, 0, sizeof(*out));
    uint64_t pa = x86_boot_framebuffer_pa;
    if (!pa)
        return 0;
    if (pa >= 0x100000000ULL || sizeof(*out) > 0x100000000ULL - pa)
        return -1;
    memcpy(out, (const void *)(uintptr_t)(PAGE_OFFSET + pa), sizeof(*out));
    return boot_framebuffer_validate(out);
}

/* Text console mapping for the firmware scanout.
 *
 * The boot direct map covers 0..4 GiB at PAGE_OFFSET, and ranges at or above
 * 2 GiB are marked uncached (PWT|PCD) for MMIO.  A scanout inside that window
 * is used directly.  Modern firmware can instead place the GOP framebuffer in
 * the iGPU VRAM BAR above 4 GiB; that needs an explicit uncached mapping,
 * which only exists once the page allocator is ready (fbcon_attach_late()).
 * fbcon never unmaps the result. */
void *arch_fbcon_map(const boot_framebuffer_t *fb) {
    if (!fb || !fb->physical || !fb->height || !fb->stride)
        return 0;
    uint64_t visible = (uint64_t)fb->stride * fb->height;
    if (!visible)
        return 0;
    /* Prefer a write-combining alias.  The GOP scanout on this class of
     * laptop lives above 4 GiB or in an uncached direct-map leaf; a 4 MiB
     * scroll on a UC mapping is what made one console line take a second.
     * Before the page allocator exists the WC alias cannot be built, so fall
     * back to the direct map and remap in fbcon_attach_late(). */
    void *mapped = arch_device_map_mmio(fb->physical, (size_t)visible,
                                        ARCH_MMIO_WC);
    if (mapped)
        return mapped;
    if (fb->physical < 0x100000000ULL &&
        visible <= 0x100000000ULL - fb->physical)
        return (void *)(uintptr_t)(fb->physical + PAGE_OFFSET);
    return 0;
}
