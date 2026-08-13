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

#endif
