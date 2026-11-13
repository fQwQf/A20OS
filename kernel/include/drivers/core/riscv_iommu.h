#ifndef _DRIVERS_CORE_RISCV_IOMMU_H
#define _DRIVERS_CORE_RISCV_IOMMU_H

#include "core/types.h"

struct device;
struct driver_dma_domain;
/* Native PCI requester -> managed DMA domain. Task context; serialize with
 * driver binding/user ownership. DMA stopped and PCI bus mastering disabled
 * before create and shutdown. No physical-DMA fallback. Native owners use
 * distinct negative tokens, never PID identities. No BME enable is performed.
 * Requires the current published device identity. Claims the explicit RID in
 * the shared PCI requester ledger before any DC/page-table publication; overlap
 * -> -EBUSY. No alias inference. Creation is serialized with discovery.
 * Uncertain invalidation retains this lease and device identity with tables.
 * Shutdown transfers a confirmed-detached lease to the managed context; final
 * domain put releases it outside the mapping mutex. Keep failed cleanup handles.
 * On creation error, non-NULL *out is a closed cleanup handle: retry shutdown,
 * then put. Never discard it or release provider hardware/module resources.
 * Hardware currently provides 8 domains, 64 pages/domain and a one-level DDT;
 * unsupported requesters are rejected. Caller uses generic mapping APIs. */
int riscv_iommu_dma_domain_create(struct device *device,
                                  struct driver_dma_domain **out);

struct riscv_iommu_dma_fault {
    uint64_t count, address;
    uint32_t cause, queue_error;
    int isolation_error;
    bool blocked, isolated;
};
/* Task-context poll; caller holds device reference and serializes with binding
 * changes. Drains the shared FQ for ALL owners, rejects further maps on each
 * faulted domain, disables BME and invalidates contexts. Failed enforcement is
 * retried on later polls. Snapshot is cumulative and never clears the latch.
 * isolated confirms BME-disable/context-invalidation commands, NOT DMA engine
 * completion or safe RAM reuse. Stop hardware before unmap/shutdown regardless.
 * Returns 0 with a snapshot (including isolation_error), -ENODEV without this
 * device object's native domain. No IRQ delivery or automatic fence signaling. */
/* queue_error latches FQCSR memory-fault/overflow bits. Such errors isolate
 * all live owners, reject new claims and do not invent missing fault records.
 * count is then only a lower bound. Hardware queue recovery is separate. */
int riscv_iommu_dma_fault_poll(struct device *device, struct riscv_iommu_dma_fault *out);

struct riscv_iommu_dma_watch;
/* Task context after scheduler start. Own worker, device reference and owner
 * token. period is a nonzero native-tick interval after each completed poll.
 * Callback runs unlocked on that worker for changed fault/isolation state;
 * healthy unchanged snapshots stay quiet. A poll error is delivered once and
 * ends polling. Snapshot is borrowed until callback returns; copy if needed.
 * Callback may run before start returns. Initialize context first and keep
 * code/module/context alive until stop succeeds. Serialize start/stop retries
 * and binding changes; do not wait for the binding lock inside the callback.
 * Failure can return a non-NULL cleanup handle if worker draining failed. */
int riscv_iommu_dma_watch_start(struct device *, uint64_t period,
                                void (*callback)(void *, int, const struct riscv_iommu_dma_fault *),
                                void *context, struct riscv_iommu_dma_watch **out);
/* Permanently stops scheduling and drains callback. Success frees/NULLs the
 * watch; deadline is absolute ticks, 0 unbounded. Timeout/interruption keeps
 * the object and context live for retry. Self-stop returns -EDEADLK without
 * freeing anything. Stop before domain shutdown/device teardown. No automatic
 * fence completion, engine reset or IRQ delivery is implied by this worker. */
int riscv_iommu_dma_watch_stop(struct riscv_iommu_dma_watch **, uint64_t deadline);

/* One device, one translation domain.  These calls fail closed when the
 * RISC-V IOMMU is absent; callers must never fall back to physical DMA for a
 * device that requested isolation. Positive PID API returns 0/-1, requires a
 * published PCI function, and participates in the SAME requester ledger as
 * native domains. Healthy idempotence requires the same device identity;
 * re-enumeration does not refresh an old claim. Claim/release are task-context
 * discovery transactions. After acquiring the lease, a new user claim disables
 * BME and verifies readback before DC publication; rejected overlaps do not
 * write config or contexts. Healthy idempotence does not change BME. Caller
 * coordinates binding/DMA engine stop; BME clear alone does not drain DMA. */
int riscv_iommu_domain_claim(uint16_t devid, int owner_pid);
/* Map only caller-owned, allocated order-0 RAM; hold those references until
 * return. The backend pins every page independently before publishing PTEs.
 * Failure zeros *out_iova. An uncertain invalidation retains pins and slots
 * and blocks further maps until successful release (or explicit unmap).
 * Clearing a PTE alone is never sufficient to release its frame reference. */
int riscv_iommu_domain_map(uint16_t devid, int owner_pid, uint64_t phys,
                           uint32_t npages, uint64_t *out_iova);
int riscv_iommu_domain_unmap(uint16_t devid, int owner_pid, uint64_t iova,
                             uint32_t npages);
int riscv_iommu_domain_fault(uint16_t devid, int owner_pid,
                            uint64_t *count, uint32_t *cause,
                            uint64_t *iova, int *blocked);
/* Caller stops the DMA engine/bus mastering first. Failure retains tables,
 * pins and the shared requester lease/device reference for retry. User leases
 * are released only after confirmed context invalidation; native ownership can
 * only be released through its managed handle, not a negative PID here.
 * Failed claim can also retain page tables;
 * the same owner's next claim first retries safe disposal of those tables.
 * A blocked successful claim must be released, not treated as idempotent. */
int riscv_iommu_domain_release(uint16_t devid, int owner_pid);

/* Global counters for /proc/a20/iommu.  All fields are cumulative since
 * probe, except mapped_pages (current, including quarantined pins/IOVAs) and
 * the last_fault_* tuple. Failed claims retained for cleanup are not counted
 * as successful claims; their eventual cleanup can increment releases. */
typedef struct riscv_iommu_stats {
    int      enabled;
    uint64_t domains_claimed;
    uint64_t domains_released;
    uint64_t maps;
    uint64_t map_failures;
    uint64_t unmaps;
    uint64_t mapped_pages;
    uint64_t fault_records;  /* every FQ record consumed, any devid */
    uint64_t faults;         /* records attributed to the user domain */
    uint64_t blocked_events; /* times a domain was fail-closed */
    uint16_t last_fault_devid;
    uint32_t last_fault_cause;
    uint64_t last_fault_iova;
    int      last_fault_owner;
} riscv_iommu_stats_t;

void riscv_iommu_get_stats(riscv_iommu_stats_t *out);

#endif
