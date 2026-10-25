/* SPDX-License-Identifier: Apache-2.0 */
#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"
#include "drivers/block/sunxi_mmc.h"

/* Firmware ID matching is provided by driver_t; no numeric platform IDs are
 * invented for DT devices. This PIO engine needs neither DMA nor a claimed IRQ. */
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE, A20_DRIVER_TYPE_BLOCK,
                      "sunxi-mmc", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO, 0, 0);

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(driver)                                                \
    uintptr_t DriverEntry(void) {                                              \
        return (uintptr_t)drv_driver_register(&(driver));                      \
    }
#include "../../drivers/block/sunxi_mmc_core.c"
#include "../../drivers/block/sunxi_mmc.c"
