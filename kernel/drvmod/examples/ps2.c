/* Module metadata/entry only; embedded uses the same shared implementation. */
#include "drvmod/drvmod.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
    A20_DRIVER_TYPE_INPUT, "ps2", A20_DRIVER_ABI,
    A20_DRIVER_RES_IOPORT | A20_DRIVER_RES_IRQ, 0, 1,
    A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED, 0x50533200UL, 0));
#undef kinfo
#undef kerr
#define kinfo(...) drv_log(__VA_ARGS__)
#define kerr(...) drv_log(__VA_ARGS__)
#undef DRIVER_REGISTER
#define DRIVER_REGISTER(drv) \
    uintptr_t DriverEntry(void) { return (uintptr_t)drv_driver_register(&(drv)); }
#include "../../drivers/input/ps2.c"
