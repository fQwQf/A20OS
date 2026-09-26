#ifndef _TIMEKEEPING_H
#define _TIMEKEEPING_H

#include "core/types.h"

void timekeeping_init(void);
void timekeeping_vdso_init(void);
void timekeeping_get_monotonic(uint64_t ts[2]);
void timekeeping_get_realtime(uint64_t ts[2]);
int  timekeeping_set_realtime(uint64_t sec, uint64_t nsec);
/* Monotonic generation counter bumped by every discontinuous realtime
 * set; used by timerfd TFD_TIMER_CANCEL_ON_SET. */
uint64_t timekeeping_realtime_set_generation(void);

#endif /* _TIMEKEEPING_H */
