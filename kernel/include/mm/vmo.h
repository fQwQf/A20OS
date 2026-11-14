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
/* Internal Linux MAP_SHARED|MAP_ANONYMOUS backing, with sparse canonical
 * slots and an immutable extent (mremap growth faults beyond it with SIGBUS). */
#define VMO_SHARED_ANON 3
/* Internal immutable RAM extent. Created only by vmo_create_contiguous;
 * its initially contiguous pages are independent canonical order-0 frames. */
#define VMO_CONTIGUOUS 4

struct a20_pager;
struct vmo_charge;

struct vmo {
    refcount_t  refcount;
    uint64_t    size;          /* logical size, bytes */
    uint64_t    phys_size;     /* allocated physical bytes */
    uint32_t    type;
    uint32_t    options;
    spinlock_t  lock;
    pfn_t      *pages;         /* canonical frames, owned by this VMO */
    void       *sparse_root;   /* SHARED_ANON: Rust-owned sparse page index */
    struct vmo_charge *shared_charges; /* SHARED_ANON: pinned per-cgroup ledger */
    uint32_t    page_count;
    struct cg_node *charge_cg; /* cgroup charged for materialized pages */
    uint64_t    charged_pages;
    struct a20_pager *pager;   /* PAGED: user-space pager (ref'ed) */
    wait_queue_t faulters;     /* PAGED: parked faulter wait queue */
};

struct vmo *vmo_create(uint32_t type, uint64_t size, uint32_t options);
/* Task context, no MM/VMO/spinlock held. Zeroed contiguous canonical RAM,
 * page-rounded immutable extent, at most PAGE_SIZE << MAX_ORDER bytes.
 * The PFA allocation is split into order-0 references before publication;
 * ordinary VMO maps, page pins and final destruction own those references.
 * Failure clears out: EINVAL parameters, E2BIG limit, ENOMEM allocation,
 * ERANGE invalid CPU alias/range, EIO failed ownership conversion. */
int vmo_create_contiguous(size_t bytes, struct vmo **out);
/* Caller retains a contiguous VMO. CPU physical range remains stable until
 * final release; not a DMA address, mapping permission or stop proof.
 * Returns EINVAL and clears outputs for other kinds/invalid parameters. */
int vmo_contiguous_info(const struct vmo *, uint64_t *physical, size_t *bytes);
void        vmo_ref(struct vmo *vmo);
void        vmo_release(struct vmo *vmo);
pfn_t       vmo_get_page(struct vmo *vmo, uint32_t index);
int64_t     vmo_resize(struct vmo *vmo, uint64_t new_size);

/* Non-materializing page lookup (M4 DMA contract): returns the canonical
 * frame for @index or PFN_NONE when not yet allocated. This is a borrowed PFN,
 * not protected against resize after return; use vmo_pin_pages for retention. */
pfn_t       vmo_peek_page(struct vmo *vmo, uint32_t index);

/* Task context, no VMO/spinlock held; caller holds a VMO reference. Atomically
 * snapshot and pin existing canonical RAM pages against resize/publication.
 * No allocation, materialization or pager calls. Anonymous, paged and sparse
 * shared-anonymous and contiguous VMOs are supported; physical/unknown kinds
 * return -EOPNOTSUPP.
 * Missing page -> -EAGAIN; invalid range/PFN/output -> -EINVAL. Errors leave
 * output unchanged and take no pins. Success writes count PFNs, each with an
 * independent reference to release via frame_put(), even if the VMO shrinks or
 * dies. Output storage must not alias VMO-owned metadata. Final-page padding
 * belongs to the page span. This is a snapshot, not a VMO extent/access lease.
 * Oversized count -> -E2BIG. Limit bounds IRQ-off traversal; larger imports
 * use page ranges. */
#define VMO_PIN_MAX_PAGES 4096U
int vmo_pin_pages(struct vmo *vmo, uint32_t first, uint32_t count, pfn_t *out);

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

#ifdef CONFIG_VMO_PAGE_RUST
/* Rust-owned page materialization transaction; these C functions are narrow
 * lock/frame/cgroup/pager primitives, not a second transaction wrapper. */
pfn_t       a20_vmo_get_page_rust(struct vmo *vmo, uint32_t index);
int         a20_vmo_get_page_charged_rust(struct vmo *vmo, uint32_t index,
                                          struct cg_node *cg, pfn_t *out);
int64_t     a20_vmo_resize_rust(struct vmo *vmo, uint64_t new_size);
void        a20_vmo_destroy_rust(struct vmo *vmo);
int         a20_vmo_page_lock(struct vmo *vmo, uint32_t index,
                              uint64_t *flags_out);
void        a20_vmo_page_unlock(struct vmo *vmo, uint64_t flags);
pfn_t       a20_vmo_page_slot_locked(struct vmo *vmo, uint32_t index);
int         a20_vmo_page_is_paged_locked(struct vmo *vmo);
int         a20_vmo_page_paged_fault(struct vmo *vmo, uint32_t index,
                                     int write_access);
int         a20_vmo_page_charge(struct vmo *vmo, struct cg_node *cg);
void        a20_vmo_page_uncharge(struct vmo *vmo, struct cg_node *cg);
pfn_t       a20_vmo_page_alloc(void);
void        a20_vmo_page_free(pfn_t pfn);
void        a20_vmo_page_zero(pfn_t pfn);
void        a20_vmo_page_publish_locked(struct vmo *vmo, uint32_t index,
                                        pfn_t pfn, struct cg_node *cg);
int         a20_vmo_resize_lock(struct vmo *vmo, uint64_t *flags_out);
void        a20_vmo_resize_unlock(struct vmo *vmo, uint64_t flags);
uint32_t    a20_vmo_resize_page_count(struct vmo *vmo);
pfn_t       a20_vmo_resize_slot(struct vmo *vmo, uint32_t index);
void       *a20_vmo_resize_alloc_table(uint32_t count);
void        a20_vmo_resize_free_table(void *table);
void        a20_vmo_resize_table_set(void *table, uint32_t index, pfn_t pfn);
void       *a20_vmo_resize_publish(struct vmo *vmo, void *table,
                                   uint32_t count, uint64_t size);
void        a20_vmo_resize_flush_tlb(void);
void        a20_vmo_resize_release_page(struct vmo *vmo, uint32_t index);
void        a20_vmo_resize_finish_shrink(struct vmo *vmo);
void        a20_vmo_destroy_notify(struct vmo *vmo);
void        a20_vmo_destroy_detach_pager(struct vmo *vmo);
uint32_t    a20_vmo_destroy_page_count(struct vmo *vmo);
pfn_t       a20_vmo_destroy_page(struct vmo *vmo, uint32_t index);
uint64_t    a20_vmo_destroy_phys_size(struct vmo *vmo);
uint64_t    a20_vmo_destroy_charged_pages(struct vmo *vmo);
void        a20_vmo_destroy_stats(uint64_t phys_size);
void        a20_vmo_destroy_flush_tlb(void);
void        a20_vmo_destroy_release_page(struct vmo *vmo, uint32_t index,
                                         pfn_t pfn);
void        a20_vmo_destroy_uncharge(struct vmo *vmo, uint64_t pages);
void        a20_vmo_destroy_free_pages(struct vmo *vmo);
void        a20_vmo_destroy_free_object(struct vmo *vmo);
#endif

#ifdef CONFIG_VMO_LIFECYCLE_RUST
/* Rust owns public VMO ref/release/peek decisions; these C functions expose
 * only atomic refcount and lock-protected field access. */
void        a20_vmo_ref_rust(struct vmo *vmo);
void        a20_vmo_release_rust(struct vmo *vmo);
pfn_t       a20_vmo_peek_page_rust(struct vmo *vmo, uint32_t index);
int         a20_vmo_pin_pages_rust(struct vmo *, uint32_t, uint32_t, pfn_t *);
uint32_t    a20_vmo_lifecycle_type_locked(struct vmo *);
uint32_t    a20_vmo_lifecycle_pin_limit(void);
int         a20_vmo_lifecycle_frame_valid(pfn_t);
void        a20_vmo_lifecycle_pin_frames(const pfn_t *, uint32_t);
void        a20_vmo_lifecycle_ref_inc(struct vmo *vmo);
int         a20_vmo_lifecycle_ref_dec_last(struct vmo *vmo);
int         a20_vmo_lifecycle_lock(struct vmo *vmo, uint64_t *flags_out);
void        a20_vmo_lifecycle_unlock(struct vmo *vmo, uint64_t flags);
uint32_t    a20_vmo_lifecycle_page_count_locked(struct vmo *vmo);
pfn_t       a20_vmo_lifecycle_slot_locked(struct vmo *vmo, uint32_t index);
#endif

#endif /* _MM_VMO_H */
