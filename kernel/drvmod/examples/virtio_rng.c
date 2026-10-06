#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

/* Three matches, because the driver binds two transports.  The virtio-mmio
 * match names the protocol type in the device-id register (there is no PCI
 * vendor to compare against); the two PCI ids are the modern
 * (virtio-1.0 only) 1af4:1044 = 0x1040 + VIRTIO_ID_RNG and the transitional
 * 1af4:1005 QEMU presents when it also offers the legacy interface.
 * Whichever binds, the driver negotiates VIRTIO_F_VERSION_1 itself. */
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_CHAR, "virtio-rng", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 3,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO, 0, 4),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1044),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1005));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/char/virtio_rng.c"