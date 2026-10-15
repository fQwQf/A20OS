#include <assert.h>
#include <proc/rt_sched_account.h>

static unsigned throttle_events;
static unsigned reschedule_requests;

uint32_t g_a20_perf_enabled = 1;
a20_perf_cpu_counters_t g_a20_perf_percpu[A20_PERF_MAX_CPUS];

unsigned arch_current_cpu_id(void)
{
    return 0;
}

void proc_sched_request_cpu(unsigned cpu, int priority)
{
    assert(cpu == 0);
    assert(priority == 0);
    reschedule_requests++;
}

int main(void)
{
    /* Enter throttle, hold it, replenish, and remain replenished. */
    a20_rt_account_effects(1, 1, 0);
    a20_rt_account_effects(0, 1, 0);
    a20_rt_account_effects(1, 0, 0);
    a20_rt_account_effects(0, 0, 0);
    throttle_events = (unsigned)g_a20_perf_percpu[0].values[A20_PERF_RT_THROTTLES];
    assert(throttle_events == 1);
    assert(reschedule_requests == 2);
    return 0;
}
