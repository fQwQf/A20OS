/* SPDX-License-Identifier: Apache-2.0 */
#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_AUDIO, "sunxi-codec", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 0);

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(driver) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(driver)); }

#include "../../drivers/audio/sunxi_audio.c"
