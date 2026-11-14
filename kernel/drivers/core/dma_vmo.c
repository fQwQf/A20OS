/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/dma_mapping.h"
#include "drivers/core/dma_mapping_internal.h"
#include "mm/vmo.h"
#include "mm/slab.h"
#include "core/errno.h"

int driver_dma_map_vmo(struct driver_dma_domain *domain, struct vmo *vmo,
                        uint32_t first, uint32_t count, enum driver_dma_direction direction,
                        struct driver_dma_mapping **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!domain || !vmo || !count ||
        (direction != DRIVER_DMA_TO_DEVICE && direction != DRIVER_DMA_FROM_DEVICE &&
         direction != DRIVER_DMA_BIDIRECTIONAL)) return -EINVAL;
    if (count > VMO_PIN_MAX_PAGES) return -E2BIG;
    size_t bytes;
    if (__builtin_mul_overflow((size_t)count, sizeof(pfn_t), &bytes)) return -EOVERFLOW;
    pfn_t *pages = kmalloc(bytes);
    if (!pages) return -ENOMEM;
    int ret = vmo_pin_pages(vmo, first, count, pages);
    if (!ret) {
        ret = driver_dma_map_pages(domain, pages, count, direction, out);
        frame_put_many(pages, count);
    }
    kfree(pages);
    return ret;
}

int driver_dma_map_owned_vmo(struct driver_dma_domain *domain, struct vmo *vmo,
                        uint32_t first, uint32_t count, enum driver_dma_direction direction,
                        struct driver_dma_mapping **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!domain || !vmo || !count ||
        (direction != DRIVER_DMA_TO_DEVICE && direction != DRIVER_DMA_FROM_DEVICE &&
         direction != DRIVER_DMA_BIDIRECTIONAL)) return -EINVAL;
    if (count > VMO_PIN_MAX_PAGES) return -E2BIG;
    if (vmo->type != VMO_RESIDENT && vmo->type != VMO_CONTIGUOUS)
        return -EOPNOTSUPP;
    /* Hold the whole backing before any provider call. A local output keeps
     * both success and cleanup handles private until the owner is installed. */
    vmo_ref(vmo);
    struct driver_dma_mapping *mapping = NULL;
    int ret = driver_dma_map_vmo(domain, vmo, first, count, direction, &mapping);
    if (mapping) {
        driver_dma_mapping_adopt_vmo(mapping, vmo);
        *out = mapping;
    } else {
        vmo_release(vmo);
    }
    return ret;
}
#endif
