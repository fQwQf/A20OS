/* Board RTC protocol shared by the C and Rust user-service shells. */
#ifndef _DRIVERS_DUAL_SERVICE_RTC_H
#define _DRIVERS_DUAL_SERVICE_RTC_H

#if defined(__aarch64__)
#include "drivers/dual/drv_env.h"
#define SERVICE_RTC_BASE 0x09010000ULL
#define SERVICE_RTC_SIZE 0x1000ULL
#define SERVICE_RTC_IRQ  34u /* PL031 SPI 2, full GIC INTID */
#define SERVICE_RTC_NAME "pl031-rtc"

static inline uint64_t service_rtc_map(void)
{
    return drv_mmio_map(SERVICE_RTC_BASE, SERVICE_RTC_SIZE, 3);
}

static inline uint64_t service_rtc_read_ns(uint64_t base)
{
    return (uint64_t)drv_mmio_read32(base, 0x00) * 1000000000ULL;
}

static inline void service_rtc_set_alarm_ns(uint64_t base, uint64_t ns)
{
    /* PL031 exposes whole seconds, with no readable fractional phase.
     * Round up and reserve one extra tick so a relative alarm derived
     * from read_ns() cannot fire early near a second boundary. */
    uint64_t seconds = ns / 1000000000ULL + (ns % 1000000000ULL != 0) + 1;
    drv_mmio_write32(base, 0x10, 0); /* mask while clearing/reprogramming */
    drv_mmio_write32(base, 0x1c, 1);
    drv_mmio_write32(base, 0x04, (uint32_t)seconds);
    drv_mmio_write32(base, 0x10, 1);
}

static inline void service_rtc_clear_alarm(uint64_t base)
{
    drv_mmio_write32(base, 0x10, 0);
    drv_mmio_write32(base, 0x1c, 1);
}
#else
#include "drivers/dual/goldfish_rtc.h"
#define SERVICE_RTC_BASE GOLDFISH_RTC_BASE
#define SERVICE_RTC_SIZE GOLDFISH_RTC_SIZE
#define SERVICE_RTC_IRQ  GOLDFISH_RTC_IRQ
#define SERVICE_RTC_NAME "goldfish-rtc"
#define service_rtc_map grtc_map
#define service_rtc_read_ns grtc_read_ns
#define service_rtc_set_alarm_ns grtc_set_alarm_ns
#define service_rtc_clear_alarm grtc_clear_alarm
#endif
#endif
