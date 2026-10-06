#ifndef _TIMEKEEPING_H
#define _TIMEKEEPING_H

#include "core/types.h"

void timekeeping_init(void);
void timekeeping_vdso_init(void);
void timekeeping_get_monotonic(uint64_t ts[2]);
void timekeeping_get_realtime(uint64_t ts[2]);
int  timekeeping_set_realtime(uint64_t sec, uint64_t nsec);
#if defined(CONFIG_X86_64)
/* Adopt a hardware wall clock (the CMOS RTC / MC146818) read by the cmos-rtc
 * driver, replacing the build-time seed timekeeping_init() had to fall back
 * to.  Only x86_64 has such a source, so this does not exist on the other
 * architectures: their wall clock still only ever comes from
 * A20_BUILD_UNIX_TIME or an explicit timekeeping_set_realtime() (which is
 * what /dev/rtc RTC_SET_TIME uses). */
void timekeeping_wallclock_set_hw(uint64_t sec);
/* Non-zero once a hardware wall clock has replaced the build-time seed. */
int  timekeeping_wallclock_from_hw(void);
#endif
/* Monotonic generation counter bumped by every discontinuous realtime
 * set; used by timerfd TFD_TIMER_CANCEL_ON_SET. */
uint64_t timekeeping_realtime_set_generation(void);

#endif /* _TIMEKEEPING_H */
