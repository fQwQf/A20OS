#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

/*
 * Loadable package shell for the r8169-family driver.  The descriptor names the
 * part this tree's target hardware has (10ec:8168); the embedded profile
 * registers the same source directly and there the driver's own id_table
 * covers the rest of the family.
 */
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_NET, "r8169", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 1, A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x10ec, 0x8168));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/net/r8169.c"
