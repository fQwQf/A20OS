#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"
#include "drivers/block/dw_sdio.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_BLOCK, "dw-sdio", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_DMA,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO,
                                       DW_SDIO_PLATFORM_VENDOR,
                                       DW_SDIO_PLATFORM_DEVICE));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/block/dw_sdio.c"
