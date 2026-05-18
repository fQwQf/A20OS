/*
 * A20OS liba20c — time functions.
 */
#include <time.h>
#include <stdint.h>
#include "../liba20rt/a20_syscall.h"

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    uint64_t ns = 0;
    int64_t r = a20_clock_get((uint32_t)clk, &ns);
    if (r < 0) return -1;
    ts->tv_sec  = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
    return 0;
}

time_t time(time_t *t)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return (time_t)-1;
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}
