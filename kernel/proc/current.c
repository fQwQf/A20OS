#include "proc/proc_internal.h"
#include "core/cpu.h"

/*
 * Current task storage.
 *
 * The kernel still boots one CPU, but current task state is already indexed by
 * CPU id. This keeps the scheduler-facing API stable for SMP bringup.
 */
static task_t *g_cpu_current[CONFIG_NR_CPUS];

task_t *proc_current(void)
{
    return g_cpu_current[cpu_current_id()];
}

task_t *proc_set_current(task_t *next)
{
    unsigned cpu = cpu_current_id();
    task_t *old = g_cpu_current[cpu];
    g_cpu_current[cpu] = next;
    return old;
}
