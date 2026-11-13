/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_DMA_BUFFER_H
#define A20_DRIVER_DMA_BUFFER_H
#include "core/types.h"
struct device;
struct driver_dma_buffer;

/* Task context, outside spinlocks. Allocates zeroed physically contiguous
 * RAM within the device's captured DMA mask. DMA address alignment is a
 * nonzero power of two; size/alignment may reach PAGE_SIZE << MAX_ORDER.
 * Records the actual allocation order, including alignment > size.
 * Caller holds a device reference and coordinates mask/submission changes.
 * No lifecycle mutex is taken: usable from an active class call. Checks of
 * registered/stopping state are snapshots, not a lease on the binding.
 * Failure clears out; EINVAL parameters/mask, E2BIG allocator limit,
 * EOPNOTSUPP architecture not ready, ENODEV unavailable owner, ENOMEM OOM,
 * ERANGE unusable allocator address, EAGAIN mask changed during allocation. */
int driver_dma_buffer_create(struct device *, size_t size, size_t alignment,
                              struct driver_dma_buffer **out);
/* Caller owns a reference. get is IRQ-safe; final put is task context.
 * Independent of devres/unbind. A device pin retains metadata, not hardware
 * or module code. Keep a reference until DMA stop is proven; put does not
 * stop hardware, invalidate mappings or wait for requests. NULL is a no-op. */
void driver_dma_buffer_get(struct driver_dma_buffer *);
void driver_dma_buffer_put(struct driver_dma_buffer *);
/* Caller holds a reference. CPU alias and CPU physical address are stable
 * until final put. The address is a bus address only on an identity-DMA
 * platform. Not an IOMMU mapping or an implicit DMA permission.
 * size is the requested byte count, not the rounded backing allocation.
 * NULL queries return zero/NULL; DMA address zero can also be valid. */
void *driver_dma_buffer_data(const struct driver_dma_buffer *);
uint64_t driver_dma_buffer_address(const struct driver_dma_buffer *);
size_t driver_dma_buffer_size(const struct driver_dma_buffer *);
#endif
