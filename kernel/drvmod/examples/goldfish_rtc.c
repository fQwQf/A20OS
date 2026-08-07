/*
 * Example driver module: goldfish RTC (QEMU virt, MMIO 0x101000).
 *
 * DriverEntry only publishes the driver description; the kernel binding
 * pass (drvmod_bind_all) matches this module against devices registered
 * by kernel init and calls rtc_probe for the match.
 */

#include "drvmod/drvmod.h"
#include <stdint.h>
#include "core/string.h"

#define GOLDFISH_RTC_BASE 0x101000UL
#define GOLDFISH_RTC_SIZE 0x100UL
#define RTC_TIME_LOW  0x00
#define RTC_TIME_HIGH 0x04

static drv_driver_t g_rtc_driver;

static int rtc_probe(drv_device_t *dev)
{
    if (!dev)
        return -1;
    uintptr_t base = dev->mmio_phys ? dev->mmio_phys : GOLDFISH_RTC_BASE;
    if (drv_map_mmio(dev, base, GOLDFISH_RTC_SIZE) < 0)
        return -1;
    uint32_t lo = drv_read32(dev, RTC_TIME_LOW);
    uint32_t hi = drv_read32(dev, RTC_TIME_HIGH);
    uint64_t t = ((uint64_t)hi << 32) | lo;
    drv_log("[GOLDFISH-RTC] probe ok: epoch=%llu\n", t);
    drv_device_register(dev);
    return 0;
}

uintptr_t DriverEntry(drv_driver_t **out)
{
    g_rtc_driver.name = "goldfish-rtc";
    g_rtc_driver.match_count = 1;
    g_rtc_driver.match[0].bus = 3;
    g_rtc_driver.match[0].vendor = GOLDFISH_RTC_BASE;
    g_rtc_driver.match[0].device = 0;
    g_rtc_driver.probe = rtc_probe;
    g_rtc_driver.remove = NULL;
    if (out)
        *out = &g_rtc_driver;
    return 0;
}
