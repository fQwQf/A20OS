/*
 * A20OS liba20c — time functions.
 */
#include <time.h>
#include <stdint.h>
#include "../liba20rt/a20_syscall.h"
#include "../liba20rt/a20_clock.h"

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    a20_time_t t;
    int64_t r = a20_clock_get((uint32_t)clk, &t);
    if (r < 0) return -1;
    ts->tv_sec  = (time_t)(t.secs);
    ts->tv_nsec = (long)(t.nsecs);
    return 0;
}

time_t time(time_t *t)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return (time_t)-1;
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}
