#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_BLOCK, "ahci", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x8086, 0x2922),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x8086, 0x2829));

/* drivers/block/ahci.c registers two drivers through DRIVER_REGISTER; the
 * ET_REL module ABI allows exactly one DriverEntry per object, so capture
 * the include and register both explicitly once. */
#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) extern char drv##_register_suppressed;

#include "../../drivers/block/ahci.c"

uintptr_t DriverEntry(void)
{
    uintptr_t id = drv_driver_register(&ahci_driver);
    if (!(id >> 63))
        drv_driver_register(&ahci_platform_driver);
    return id;
}
