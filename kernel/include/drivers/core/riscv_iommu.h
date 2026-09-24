#ifndef _DRIVERS_CORE_RISCV_IOMMU_H
#define _DRIVERS_CORE_RISCV_IOMMU_H

#include "core/types.h"

/* One device, one translation domain.  These calls fail closed when the
 * RISC-V IOMMU is absent; callers must never fall back to physical DMA for a
 * device that requested isolation. */
int riscv_iommu_domain_claim(uint16_t devid, int owner_pid);
int riscv_iommu_domain_map(uint16_t devid, int owner_pid, uint64_t phys,
                           uint32_t npages, uint64_t *out_iova);
int riscv_iommu_domain_unmap(uint16_t devid, int owner_pid, uint64_t iova,
                             uint32_t npages);
int riscv_iommu_domain_fault(uint16_t devid, int owner_pid,
                            uint64_t *count, uint32_t *cause,
                            uint64_t *iova, int *blocked);
int riscv_iommu_domain_release(uint16_t devid, int owner_pid);

/* Global counters for /proc/a20/iommu.  All fields are cumulative since
 * probe, except mapped_pages (current) and the last_fault_* tuple. */
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
