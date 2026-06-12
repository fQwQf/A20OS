#include "proc/proc_internal.h"
#include "core/cpu.h"

/*
 * Current task storage.
 *
 * The kernel still boots one CPU, but current task state is already indexed by
 * CPU id. This keeps the scheduler-facing API stable for SMP bringup.
 *
 * PER_CPU_CURRENT_VALIDATION:
 * - CPU 0 sets current during proc_init(); every secondary CPU must set its own
 *   slot exactly once before enabling preemption or taking scheduler IPIs.
 * - proc_set_current() is the only writer for g_cpu_current[cpu]. A context
 *   switch changes exactly the current CPU slot and never another CPU's slot.
 * - Cross-CPU wakeup and IPI reschedule tests must prove that cpu_current_id(),
 *   runqueue selection, and current slot lookup agree for the CPU handling the
 *   interrupt.
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
