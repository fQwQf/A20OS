/* SPDX-License-Identifier: Apache-2.0 */
#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE, A20_DRIVER_TYPE_BLOCK,
                      "sdhci", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO, 0, 0);

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv)                                                   \
    uintptr_t DriverEntry(void) {                                              \
        return (uintptr_t)drv_driver_register(&(drv));                         \
    }

#include "../../drivers/block/sdhci_core.c"
#include "../../drivers/block/sdhci.c"
