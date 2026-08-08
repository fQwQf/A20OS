#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_BLOCK, "ahci", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x8086, 0x2922),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x8086, 0x2829));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/block/ahci.c"
