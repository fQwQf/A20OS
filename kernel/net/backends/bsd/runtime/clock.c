/* F-Stack's callout timecounter must use A20OS's monotonic clock, not
 * rte_rdtsc/rte_get_tsc_hz or floating point. Wall-clock corrections must not
 * move TCP retransmission timers. Keep BSD declarations private to this port. */
#include "core/timekeeping.h"
#include "ff_host_interface.h"

void ff_clock_gettime(int id, int64_t *sec, long *nsec)
{
    uint64_t ts[2];
    if (id == ff_CLOCK_REALTIME)
        timekeeping_get_realtime(ts);
    else
        /* Match the upstream contract: MONOTONIC_FAST and unknown IDs use
         * monotonic time. Neither call depends on worker refresh cadence. */
        timekeeping_get_monotonic(ts);
    *sec = (int64_t)ts[0];
    *nsec = (long)ts[1];
}

uint64_t ff_clock_gettime_ns(int id)
{
    int64_t sec;
    long nsec;
    ff_clock_gettime(id, &sec, &nsec);
    return (uint64_t)sec * ff_NSEC_PER_SEC + (uint64_t)nsec;
}

uint64_t ff_get_tsc_ns(void)
{
    /* The upstream name is historical: callers require elapsed nanoseconds,
     * not raw TSC ticks. A20OS owns clock frequency and boot-time conversion. */
    return ff_clock_gettime_ns(ff_CLOCK_MONOTONIC);
}

void ff_get_current_time(int64_t *sec, long *nsec)
{
    uint64_t ts[2];
    timekeeping_get_realtime(ts);
    if (sec) *sec = (int64_t)ts[0];
    if (nsec) *nsec = (long)ts[1];
}
