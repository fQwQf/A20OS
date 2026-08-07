/*
 * Goldfish RTC — shared device protocol (dual-placement).
 *
 * Single source for the register map and register-level protocol of the
 * QEMU virt goldfish RTC (hw/rtc/goldfish_rtc.c, Linux goldfish driver).
 * Both the kernel-mode driver (the drvmod module
 * kernel/drvmod/examples/goldfish_rtc.c) and the user-space driver
 * (user/svc/rtcd.c) are built on this header;
 * placement comes from drv_env.h (DRV_ENV_KERNEL / DRV_ENV_USER).
 */
#ifndef _DRIVERS_DUAL_GOLDFISH_RTC_H
#define _DRIVERS_DUAL_GOLDFISH_RTC_H

#include "drivers/dual/drv_env.h"

#define GOLDFISH_RTC_BASE   0x101000ULL
#define GOLDFISH_RTC_SIZE   0x1000ULL
#define GOLDFISH_RTC_IRQ    11u

#define GRTC_TIME_LOW       0x00u   /* R: nanoseconds low/high */
#define GRTC_TIME_HIGH      0x04u
#define GRTC_ALARM_LOW      0x08u   /* RW */
#define GRTC_ALARM_HIGH     0x0cu
#define GRTC_IRQ_ENABLE     0x10u   /* RW */
#define GRTC_CLEAR_ALARM    0x14u   /* W */
#define GRTC_ALARM_STATUS   0x18u   /* R */

static inline uint64_t grtc_map(void)
{
    return drv_mmio_map(GOLDFISH_RTC_BASE, GOLDFISH_RTC_SIZE, 3 /* RW */);
}

static inline uint64_t grtc_read_ns(uint64_t base)
{
    uint32_t lo, hi;
    do {
        hi = drv_mmio_read32(base, GRTC_TIME_HIGH);
        lo = drv_mmio_read32(base, GRTC_TIME_LOW);
    } while (hi != drv_mmio_read32(base, GRTC_TIME_HIGH));
    return ((uint64_t)hi << 32) | lo;
}

static inline void grtc_set_alarm_ns(uint64_t base, uint64_t ns)
{
    drv_mmio_write32(base, GRTC_ALARM_HIGH, (uint32_t)(ns >> 32));
    drv_mmio_write32(base, GRTC_ALARM_LOW, (uint32_t)ns);
    drv_mmio_write32(base, GRTC_IRQ_ENABLE, 1);
}

static inline void grtc_clear_alarm(uint64_t base)
{
    drv_mmio_write32(base, GRTC_CLEAR_ALARM, 0);
}

#endif
