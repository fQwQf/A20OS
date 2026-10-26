#ifndef A20_DRM_SCANOUT_MAPPING_H
#define A20_DRM_SCANOUT_MAPPING_H

#include "boot/context.h"
#include "core/errno.h"
#include "drivers/core/driver_core.h"
#include "mm/frame.h"

typedef struct drm_scanout_mapping {
    void *address;
    size_t bytes;
    int mmio;
} drm_scanout_mapping_t;

/* PFNs are compact allocator indices, not physical page numbers. Retained
 * scanouts are explicitly excluded from that allocator. Preserve the boot
 * framebuffer's verified cache attributes; ordinary GPU RAM and VRAM each
 * use their own mapping contract instead of treating every PA as a PFN. */
static inline int drm_scanout_acquire(uintptr_t physical, size_t bytes,
                                       drm_scanout_mapping_t *map) {
    if (!map) return -EINVAL;
    *map = (drm_scanout_mapping_t){0};
    if (!bytes || (physical & (PAGE_SIZE - 1)) || bytes > (uintptr_t)-1 - physical)
        return -EINVAL;
    const boot_framebuffer_t *fb = &boot_context_get()->framebuffer;
    if (fb->magic && physical == fb->physical) {
        if (bytes > fb->aperture_bytes) return -EINVAL;
        map->address = boot_framebuffer_map();
        if (!map->address) return -ENXIO; /* Never retry RAM with Device attrs. */
    } else {
        const pfa_range_t *range = pfa_range_for_pa(physical);
        if (range) {
            if (bytes > range->end - physical) return -EINVAL;
            map->address = pfn_to_virt(phys_to_pfn(physical));
        } else {
            /* A failed RAM span must never be retried as uncached MMIO. */
            for (size_t i = 0; i < pfa.nr_ranges; i++)
                if (physical < pfa.ranges[i].end &&
                    pfa.ranges[i].base < physical + bytes)
                    return -EINVAL;
            map->address = arch_device_map_mmio(physical, bytes, 0);
            map->mmio = map->address != NULL;
        }
        if (!map->address) return -ENXIO;
    }
    map->bytes = bytes;
    return 0;
}

static inline void drm_scanout_release(drm_scanout_mapping_t *map) {
    if (map->mmio && map->address)
        arch_device_unmap_mmio(map->address, map->bytes);
    *map = (drm_scanout_mapping_t){0};
}

#endif
