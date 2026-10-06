#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

/* Two PCI ids: the modern (virtio-1.0 only) 1af4:1043 = 0x1040 +
 * VIRTIO_ID_CONSOLE, and the transitional 1af4:1003 QEMU presents when it
 * also offers the legacy interface.  Both are matched; whichever binds, the
 * driver then negotiates VIRTIO_F_VERSION_1 itself. */
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_CHAR, "virtio-console", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1043),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1003));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/char/virtio_console.c"
