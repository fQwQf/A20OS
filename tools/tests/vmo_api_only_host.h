/* Queue/requester fixtures compile but never execute the VMO/EDU selftest.
 * Production VMO snapshot integration lives in test_driver_dma_vmo. */
#pragma once
#include "core/types.h"
#include "mm/frame.h"
#define VMO_ANONYMOUS 0
struct vmo;
struct vmo *vmo_create(uint32_t, uint64_t, uint32_t);
int vmo_create_contiguous(size_t, struct vmo **);
void vmo_release(struct vmo *);
pfn_t vmo_get_page(struct vmo *, uint32_t);
