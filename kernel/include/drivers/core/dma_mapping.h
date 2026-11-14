/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_DMA_MAPPING_H
#define A20_DRIVER_DMA_MAPPING_H

#include "core/types.h"
#include "mm/frame.h"

struct device;
struct driver_dma_domain;
struct driver_dma_mapping;
struct driver_dma_buffer;

enum driver_dma_direction {
    DRIVER_DMA_TO_DEVICE,
    DRIVER_DMA_FROM_DEVICE,
    DRIVER_DMA_BIDIRECTIONAL,
};

/* Immutable physical-memory restrictions, independent of device/firmware
 * storage and hardware ownership. Access is from the DEVICE's perspective. */
#define DRIVER_DMA_POLICY_READ  1U
#define DRIVER_DMA_POLICY_WRITE 2U
#define DRIVER_DMA_POLICY_IDENTITY 1U
#define DRIVER_DMA_POLICY_MAX_RULES 1024U
struct driver_dma_policy;
struct driver_dma_rule {
    uint64_t start, end; /* Inclusive physical bytes; end may be UINT64_MAX. */
    uint32_t access; /* READ/WRITE, or zero to deny all DMA. */
    uint32_t flags;  /* IDENTITY requires IOVA == physical address. */
};
/* Task context. Copies up to MAX_RULES; zero rules is unrestricted. Overlap
 * intersects access and combines identity requirements; gaps are unrestricted.
 * Rules remain restrictions, never permission to access memory or enable DMA.
 * Failure clears out; bad rules -> -EINVAL, excessive count -> -E2BIG. */
int driver_dma_policy_create(const struct driver_dma_rule *, size_t count,
                              struct driver_dma_policy **out);
void driver_dma_policy_get(struct driver_dma_policy *);
void driver_dma_policy_put(struct driver_dma_policy *);
/* Caller holds policy reference. Checks every intersecting byte interval.
 * TO_DEVICE requires READ, FROM_DEVICE WRITE, bidirectional both. Any denied
 * byte -> -EACCES; any identity rule makes the whole requested span identity.
 * Error clears identity. No page rounding, allocation or hardware access. */
int driver_dma_policy_check(const struct driver_dma_policy *, uint64_t physical,
                             uint64_t length, enum driver_dma_direction,
                             bool *identity);

/* Native, page-granular RAM mappings. A provider maps exactly PAGE_SIZE
 * bytes per call; failure must leave that page unmapped. unmap_page must
 * synchronously invalidate device translations before returning. Providers
 * must permit concurrent calls for independent mappings. The ops and their
 * module, if any, must remain resident until release(context) returns.
 * RAM is borrowed from the mapping until confirmed unmap. A physical page may
 * be an order-N allocation tail: providers must not infer order-0 ownership or
 * frame_get/put it. release owns context disposal, not the device or RAM. */
struct driver_dma_ops {
    int (*map_page)(void *context, uint64_t physical,
                    enum driver_dma_direction direction, uint64_t *address);
    void (*unmap_page)(void *context, uint64_t address,
                       enum driver_dma_direction direction);
    void (*release)(void *context);
};

/* Fallible invalidation provider. A successful unmap confirms translations
 * are no longer usable and the IOVA may be recycled. Failure MUST reserve
 * that address until retry succeeds, even if its PTE was already cleared.
 * Retry must accept that partially completed operation. map failure still
 * guarantees no translation or outstanding invalidation for that page.
 * release is infallible software disposal after all mappings were drained;
 * hardware domain detach must be coordinated separately by the provider. */
struct driver_dma_checked_ops {
    int (*map_page)(void *, uint64_t, enum driver_dma_direction, uint64_t *);
    int (*unmap_page)(void *, uint64_t, enum driver_dma_direction);
    void (*release)(void *);
};

/* Hardware-owned domain with uncertain map failures and fallible detach.
 * map_page sets needs_unmap=true whenever it installed/reserved an address,
 * including failures. That address must remain valid for unmap retry until
 * unmap succeeds. Success requires needs_unmap=true. Failure with false
 * guarantees no residual translation/invalidation for that page.
 * deactivate runs only after every mapping/cleanup handle is drained; success
 * confirms hardware no longer references provider resources. release then
 * disposes software context on the final put. All callbacks are task context,
 * serialized per domain, and must not reenter this domain's map/unmap/shutdown. */
struct driver_dma_managed_ops {
    int (*map_page)(void *, uint64_t, enum driver_dma_direction, uint64_t *, bool *needs_unmap);
    int (*unmap_page)(void *, uint64_t, enum driver_dma_direction);
    int (*deactivate)(void *);
    void (*release)(void *);
};

/* Task context, outside spinlocks. Success transfers one context ownership
 * reference to the domain; failure leaves it with the caller. The domain
 * pins the device object and snapshots its DMA mask. It does not keep a
 * driver binding alive. Serialize creation/submission with device teardown. */
int driver_dma_domain_create(struct device *device,
                             const struct driver_dma_ops *ops, void *context,
                             struct driver_dma_domain **out);
int driver_dma_domain_create_checked(struct device *device,
                                     const struct driver_dma_checked_ops *ops,
                                     void *context, struct driver_dma_domain **out);
/* Success retains an internal provider reference until shutdown succeeds.
 * Final owner put without shutdown therefore leaks safely, never detaches or
 * frees live hardware context. Caller must retain a reference for retry. */
int driver_dma_domain_create_managed(struct device *device,
                                     const struct driver_dma_managed_ops *ops,
                                     void *context, struct driver_dma_domain **out);
/* Caller holds a domain reference and has stopped DMA. Permanently closes
 * new maps, returns -EBUSY while mapping/cleanup handles remain; unmap them
 * then retry. Deactivation failure retains context/internal reference for
 * retry. Success is idempotent and does not consume caller's reference.
 * Only managed domains support shutdown; others return -EOPNOTSUPP. */
int driver_dma_domain_shutdown(struct driver_dma_domain *domain);
/* Explicit opt-in ONLY for a platform configured for identity DMA. This is
 * never a fallback after an IOMMU/provider error. No isolation is promised. */
int driver_dma_domain_create_direct(struct device *device,
                                    struct driver_dma_domain **out);
/* Same managed lifetime, with an independently retained immutable policy.
 * Preflights ALL physical pages before any provider call/pin. A returned IOVA
 * violating identity -> -ERANGE and normal fallible rollback (possibly a
 * retained cleanup handle). Provider must honor access and identity BEFORE
 * publishing translations; it can query the same policy from its context.
 * Core validation is not hardware PTE inspection or automatic unity mapping.
 * Success retains policy; failure consumes neither context nor policy. */
int driver_dma_domain_create_managed_policy(struct device *,
                    const struct driver_dma_managed_ops *, void *context,
                    struct driver_dma_policy *, struct driver_dma_domain **out);
void driver_dma_domain_get(struct driver_dma_domain *domain);
void driver_dma_domain_put(struct driver_dma_domain *domain);

/* Caller owns each order-0 RAM frame until this call returns; mapping takes
 * an independent reference on each frame and the domain. Never use MMIO,
 * reserved VRAM, unmaterialized VMO slots or higher-order allocation tails.
 * Each page gets a device-visible address, which may differ from its PA.
 * Success performs initial CPU->device cache synchronization. Failure rolls
 * back translations and pins. If checked rollback fails, *out is a cleanup
 * handle: caller MUST retain it and retry try_unmap, even though map returned
 * an error. It cannot be synced, queried for addresses or submitted. Original
 * map errno is preserved; try_unmap returns the current cleanup error.
 * Infallible providers keep the original NULL-on-failure contract. */
int driver_dma_map_pages(struct driver_dma_domain *domain, const pfn_t *pages,
                          size_t count, enum driver_dma_direction direction,
                          struct driver_dma_mapping **out);
/* Maps the requested buffer span, rounded up to whole pages. The final page's
 * zeroed allocation padding is also exposed; stronger-alignment backing beyond
 * that span is not mapped. Holds ONE buffer reference, never frame_get/put on
 * higher-order tails. Caller holds the buffer throughout this call. Allocation
 * owner and domain device may differ: allocation is storage, not DMA permission.
 * Domain policy checks every exposed physical byte before provider calls.
 * Returned addresses need not be contiguous; query each page. Uses the same
 * sync/address/try_unmap APIs and fallible cleanup contract as map_pages.
 * Caller coordinates CPU/device access across ALL mappings of this buffer.
 * Unmap failure retains the entire buffer, including unexposed backing. */
int driver_dma_map_buffer(struct driver_dma_domain *, struct driver_dma_buffer *,
                           enum driver_dma_direction, struct driver_dma_mapping **out);
/* Number of retained page entries, including during failed cleanup. A buffer
 * mapping retains its whole allocation regardless of this count. */
size_t driver_dma_mapping_count(const struct driver_dma_mapping *mapping);
int driver_dma_page_address(const struct driver_dma_mapping *mapping,
                             size_t index, uint64_t *out);
/* Exclusive ownership transitions: CPU/device must have stopped accessing
 * the mapping before synchronization. These functions do not wait for DMA. */
int driver_dma_mapping_sync_for_device(struct driver_dma_mapping *mapping);
int driver_dma_mapping_sync_for_cpu(struct driver_dma_mapping *mapping);
/* Task context, after DMA has stopped. Invalidates translations, syncs writes
 * back to the CPU and drops frame pins or the buffer on success, consuming
 * *mapping to NULL.
 * A mapping is single-owner; serialize sync/address/unmap against each other.
 * No automatic devres release: a timeout must never free active DMA memory. */
void driver_dma_unmap_pages(struct driver_dma_mapping **mapping);
/* Required for checked providers. Success consumes *mapping. Failure retains
 * all frame pins or the whole buffer, domain and pointer, disabling address/sync
 * with -ESHUTDOWN. Retry resumes at the failed page; previously invalidated
 * pages are not unmapped twice. Caller must keep hardware stopped throughout.
 * NULL pointer argument is -EINVAL; an already NULL mapping succeeds.
 * Legacy unmap above also preserves failed mappings but discards status. */
int driver_dma_try_unmap_pages(struct driver_dma_mapping **mapping);

#endif
