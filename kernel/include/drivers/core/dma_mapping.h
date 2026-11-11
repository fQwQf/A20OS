/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_DMA_MAPPING_H
#define A20_DRIVER_DMA_MAPPING_H

#include "core/types.h"
#include "mm/frame.h"

struct device;
struct driver_dma_domain;
struct driver_dma_mapping;

enum driver_dma_direction {
    DRIVER_DMA_TO_DEVICE,
    DRIVER_DMA_FROM_DEVICE,
    DRIVER_DMA_BIDIRECTIONAL,
};

/* Native, page-granular RAM mappings. A provider maps exactly PAGE_SIZE
 * bytes per call; failure must leave that page unmapped. unmap_page must
 * synchronously invalidate device translations before returning. Providers
 * must permit concurrent calls for independent mappings. The ops and their
 * module, if any, must remain resident until release(context) returns.
 * release owns provider-context disposal, not the device or RAM pages. */
struct driver_dma_ops {
    int (*map_page)(void *context, uint64_t physical,
                    enum driver_dma_direction direction, uint64_t *address);
    void (*unmap_page)(void *context, uint64_t address,
                       enum driver_dma_direction direction);
    void (*release)(void *context);
};

/* Task context, outside spinlocks. Success transfers one context ownership
 * reference to the domain; failure leaves it with the caller. The domain
 * pins the device object and snapshots its DMA mask. It does not keep a
 * driver binding alive. Serialize creation/submission with device teardown. */
int driver_dma_domain_create(struct device *device,
                             const struct driver_dma_ops *ops, void *context,
                             struct driver_dma_domain **out);
/* Explicit opt-in ONLY for a platform configured for identity DMA. This is
 * never a fallback after an IOMMU/provider error. No isolation is promised. */
int driver_dma_domain_create_direct(struct device *device,
                                    struct driver_dma_domain **out);
void driver_dma_domain_get(struct driver_dma_domain *domain);
void driver_dma_domain_put(struct driver_dma_domain *domain);

/* Caller owns each order-0 RAM frame until this call returns; mapping takes
 * an independent reference on each frame and the domain. Never use MMIO,
 * reserved VRAM, unmaterialized VMO slots or higher-order allocation tails.
 * Each page gets a device-visible address, which may differ from its PA.
 * Success performs initial CPU->device cache synchronization. Failures roll
 * back every translation and pin; *out is NULL. No fixed page-count limit. */
int driver_dma_map_pages(struct driver_dma_domain *domain, const pfn_t *pages,
                          size_t count, enum driver_dma_direction direction,
                          struct driver_dma_mapping **out);
size_t driver_dma_mapping_count(const struct driver_dma_mapping *mapping);
int driver_dma_page_address(const struct driver_dma_mapping *mapping,
                             size_t index, uint64_t *out);
/* Exclusive ownership transitions: CPU/device must have stopped accessing
 * the mapping before synchronization. These functions do not wait for DMA. */
int driver_dma_mapping_sync_for_device(struct driver_dma_mapping *mapping);
int driver_dma_mapping_sync_for_cpu(struct driver_dma_mapping *mapping);
/* Task context, after DMA has stopped. Invalidates translations, syncs writes
 * back to the CPU and drops pins. Consumes *mapping and sets it to NULL.
 * A mapping is single-owner; serialize sync/address/unmap against each other.
 * No automatic devres release: a timeout must never free active DMA memory. */
void driver_dma_unmap_pages(struct driver_dma_mapping **mapping);

#endif
