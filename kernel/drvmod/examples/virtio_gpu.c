#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_DISPLAY, "virtio-gpu", A20_DRIVER_ABI,
                      A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO, 0, 16),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1050));

#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }

#include "../../drivers/gpu/virtio_gpu.c"
