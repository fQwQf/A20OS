#ifndef _TIMEKEEPING_H
#define _TIMEKEEPING_H

#include "core/types.h"

void timekeeping_init(void);
void timekeeping_get_monotonic(uint64_t ts[2]);
void timekeeping_get_realtime(uint64_t ts[2]);
int  timekeeping_set_realtime(uint64_t sec, uint64_t nsec);

#endif /* _TIMEKEEPING_H */
