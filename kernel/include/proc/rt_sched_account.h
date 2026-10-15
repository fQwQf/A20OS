#ifndef A20_KERNEL_PROC_RT_SCHED_ACCOUNT_H
#define A20_KERNEL_PROC_RT_SCHED_ACCOUNT_H

#include "core/perf.h"

/* C-side effects shared by both the direct Rust accounting adapter and the
 * combined Rust pick/account adapter. The transition always requests a
 * reschedule; the counter records only entry into throttled state. */
void proc_sched_request_cpu(unsigned cpu, int priority);

static inline void a20_rt_account_effects(int changed, int throttled,
                                         unsigned cpu)
{
    if (!changed)
        return;
    if (throttled)
        a20_perf_add(A20_PERF_RT_THROTTLES, 1);
    proc_sched_request_cpu(cpu, 0);
}

#endif
