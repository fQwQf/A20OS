/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_RISCV_IOMMU_INTERNAL_H
#define A20_RISCV_IOMMU_INTERNAL_H
#include "drivers/core/riscv_iommu.h"
/* Kernel-internal owner-generation checks; not module exports. */
int riscv_iommu_native_identity(struct device *, uint16_t *devid, int *owner);
int riscv_iommu_fault_poll_owned(struct device *, int expected_owner,
                                 struct riscv_iommu_dma_fault *out);
#endif
