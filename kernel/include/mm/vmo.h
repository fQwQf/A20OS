#ifndef _MM_VMO_H
#define _MM_VMO_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
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
 *
 * PAGED VMOs (VMO_PAGED) with ->pager set hand unmaterialized-page faults to
 * the user-space pager (kernel/ipc/a20_pager.c): the faulting thread enqueues
 * a page-request message on the pager's request channel, then parks on
 * ->faulters until a20_pager_supply_pages() materializes the page and wakes
 * it.  ->pager is a reference to an a20_pager object, so it stays valid as
 * long as the VMO is alive.  Faulters must never hold ->lock while parked.
 */

#define VMO_ANONYMOUS 0
#define VMO_PHYSICAL  1
#define VMO_PAGED     2

struct a20_pager;

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
    struct a20_pager *pager;   /* PAGED: user-space pager (ref'ed) */
    wait_queue_t faulters;     /* PAGED: parked faulter wait queue */
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

/* PAGED-VMO fault: request the page from the user-space pager and park until
 * a20_pager_supply_pages() materializes it.  Caller must NOT hold the VMO's
 * mm->lock (parking under it would deadlock the pager).  Returns 0 when the
 * page is now materialized, -EAGAIN (no pager, run the zero-fill path),
 * -EINTR (killed), or -EIO (pager request peer closed). */
int         vmo_paged_fault(struct vmo *vmo, uint32_t index, int write_access);

#endif /* _MM_VMO_H */
