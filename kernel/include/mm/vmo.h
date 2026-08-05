#ifndef _MM_VMO_H
#define _MM_VMO_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/frame.h"

/*
 * VMO (Virtual Memory Object) — core memory object.
 *
 * A VMO is a reference-counted container of physical frames that multiple
 * address spaces can map.  It is the shared "page source" behind the Native
 * ABI's vm_map/vm_share and is wired into the core VMA/fault machinery as a
 * VM_VMO mapping: mapped frames are owned by the VMO (like page-cache pages),
 * faulting materializes them on demand, and fork shares the same canonical
 * frames instead of copy-on-writing them.
 *
 * LIFETIME / OWNERSHIP:
 * - vmo_create() returns a VMO with refcount 1.  Every VM_VMO VMA holds one
 *   reference, taken by mm_mmap_vmo() and dropped by vma_release().
 * - vmo_release() drops one reference; the final reference frees every frame
 *   the VMO owns.
 * - Frames in ->pages are owned solely by the VMO.  A mapped PTE must never
 *   frame_put() a VMO frame; unmapping just drops the VMA's VMO reference.
 *   This mirrors page-cache frames: the canonical frame outlives any single
 *   mapping.
 * - All VMO frames are zero-initialized on first materialization.
 */

#define VMO_ANONYMOUS 0
#define VMO_PHYSICAL  1
#define VMO_PAGED     2

struct vmo {
    refcount_t  refcount;
    uint64_t    size;          /* logical size, bytes */
    uint64_t    phys_size;     /* allocated physical bytes */
    uint32_t    type;
    uint32_t    options;
    spinlock_t  lock;
    pfn_t      *pages;         /* canonical frames, owned by this VMO */
    uint32_t    page_count;
    struct cg_node *charge_cg; /* cgroup charged for materialized pages */
    uint64_t    charged_pages;
};

struct vmo *vmo_create(uint32_t type, uint64_t size, uint32_t options);
void        vmo_ref(struct vmo *vmo);
void        vmo_release(struct vmo *vmo);
pfn_t       vmo_get_page(struct vmo *vmo, uint32_t index);
int64_t     vmo_resize(struct vmo *vmo, uint64_t new_size);

/* Non-materializing page lookup (M4 DMA contract): returns the canonical
 * frame for @index or PFN_NONE when not yet allocated. */
pfn_t       vmo_peek_page(struct vmo *vmo, uint32_t index);

/* Like vmo_get_page, but charges the frame to @cg on first materialization.
 * Returns 0 on success (pfn in *out), -ENOMEM on charge/alloc failure. */
int         vmo_get_page_charged(struct vmo *vmo, uint32_t index,
                                 struct cg_node *cg, pfn_t *out);

#endif /* _MM_VMO_H */
