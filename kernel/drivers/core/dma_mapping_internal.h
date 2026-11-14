/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DMA_MAPPING_INTERNAL_H
#define A20_DMA_MAPPING_INTERNAL_H
struct driver_dma_mapping;
struct vmo;
/* Core-only handoff. The VMO frontend owns this unpublished, exclusive mapping
 * and transfers exactly one VMO reference after page mapping has returned.
 * Accepts either a usable mapping or a failed-rollback cleanup handle. Never
 * called on a mapping which already has an owner; this cannot fail/allocate.
 * This helper is not a module API or a second backing registry. */
void driver_dma_mapping_adopt_vmo(struct driver_dma_mapping *, struct vmo *);
#endif
