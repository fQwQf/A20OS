/* Module metadata/entry only; embedded uses the same shared implementation. */
#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
    A20_DRIVER_TYPE_BLOCK, "nvme", A20_DRIVER_ABI,
    A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ | A20_DRIVER_RES_DMA, 0, 1,
    A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0xFFFFFFFFU, 0xFFFFFFFFU));
#undef kinfo
#undef kerr
#define kinfo(...) drv_log(__VA_ARGS__)
#define kerr(...) drv_log(__VA_ARGS__)
#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }
#include "../../drivers/block/nvme.c"
