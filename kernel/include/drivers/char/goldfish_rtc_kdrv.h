/*
 * goldfish_rtc_kdrv.h — kernel placement of the shared goldfish RTC driver.
 */
#ifndef _DRIVERS_CHAR_GOLDFISH_RTC_KDRV_H
#define _DRIVERS_CHAR_GOLDFISH_RTC_KDRV_H

#include "core/types.h"

/* Probe the device in kernel placement and log the current time.
 * Returns 0 on success, negative if the window reads back as absent. */
int      goldfish_rtc_kdrv_probe(void);
uint64_t goldfish_rtc_kdrv_read_ns(void);

#endif
