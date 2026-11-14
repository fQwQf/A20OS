/* Queue/requester fixtures compile but never execute the VMO/EDU selftest.
 * Production snapshots and resident factories execute in test_driver_dma_vmo
 * and test_vmo_resident; this header models no VMO layout. */
#pragma once
#include "core/types.h"
#include "mm/frame.h"
#define VMO_ANONYMOUS 0
struct vmo;
struct cg_node;
struct vmo *vmo_create(uint32_t, uint64_t, uint32_t);
int vmo_create_contiguous(size_t, struct vmo **);
int vmo_create_resident(size_t, struct cg_node *, struct vmo **);
pfn_t vmo_peek_page(struct vmo *, uint32_t);
int64_t vmo_resize(struct vmo *, uint64_t);
void vmo_release(struct vmo *);
pfn_t vmo_get_page(struct vmo *, uint32_t);
