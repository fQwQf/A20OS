#ifndef _STM32F103_RTC_H
#define _STM32F103_RTC_H

#include "core/types.h"

/* Start the backup-domain RTC, using LSE when available and LSI otherwise. */
void stm32_rtc_init(void);

/* Read or set the RTC time-of-day. */
void stm32_rtc_get_hhmmss(int *hour, int *minute, int *second);
int stm32_rtc_set_hhmmss(int hour, int minute, int second);
int stm32_rtc_set_network_time(uint32_t unix_utc, int utc_offset_minutes);
int stm32_rtc_available(void);
int stm32_rtc_network_synced(void);
uint32_t stm32_rtc_sync_count(void);
uint32_t stm32_rtc_last_unix_utc(void);
int stm32_rtc_utc_offset_minutes(void);

/* Small pure helpers, also exercised by the host smart-hub test. */
uint32_t rtc_hms_to_secs(int hour, int minute, int second);
void rtc_secs_to_hms(uint32_t seconds, int *hour, int *minute, int *second);
uint32_t rtc_unix_to_local_secs(uint32_t unix_utc, int utc_offset_minutes);

#endif /* _STM32F103_RTC_H */
