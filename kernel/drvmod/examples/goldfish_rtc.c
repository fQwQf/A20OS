/*
 * Example driver module: goldfish RTC (QEMU virt, MMIO 0x101000).
 *
 * Registers the SAME unified driver_t model as built-in drivers through
 * drv_driver_register(); the driver core binds it to the goldfish-rtc
 * platform device the kernel registered.  The probe is read-only: the
 * full device ownership belongs to the user-space rtcd service
 * (dual-placement), so this kernel shell only reads the clock.
 */

#include "drvmod/drvmod.h"
#include <stdint.h>
#include "core/string.h"
#include "core/errno.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"

#define GOLDFISH_RTC_BASE 0x101000UL
#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_RTC, "goldfish-rtc", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           GOLDFISH_RTC_BASE, 0));

static int rtc_probe(device_t *dev)
{
    if (!dev)
        return -ENODEV;
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start)
        return -ENODEV;
    uintptr_t va = drv_map_mmio(dev, (uintptr_t)res->start,
                                (size_t)(res->end - res->start + 1));
    if (!va)
        return -EIO;
    uint32_t lo = drv_read32(dev, RTC_TIME_LOW);
    uint32_t hi = drv_read32(dev, RTC_TIME_HIGH);
    uint64_t t = ((uint64_t)hi << 32) | lo;
    drv_log("[GOLDFISH-RTC] probe ok: epoch=%llu\n", t);
    return 0;
}

static int rtc_remove(device_t *dev)
{
    if (dev)
        drv_unmap_mmio(dev);
    return 0;
}

static const device_id_t rtc_ids[] = {
    { .vendor = GOLDFISH_RTC_BASE, .device = 0,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t rtc_driver = {
    .name = "goldfish-rtc",
    .id_table = rtc_ids,
    .bus = &platform_bus,
    .read_only_probe = 1,   /* user service rtcd owns the device */
    .probe = rtc_probe,
    .remove = rtc_remove,
    .class_type = DEV_CLASS_NONE,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&rtc_driver);
    drv_log("[GOLDFISH-RTC] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
