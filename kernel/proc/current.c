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
static task_t *g_cpu_switching_out[CONFIG_NR_CPUS];

task_t *proc_current(void)
{
    return g_cpu_current[cpu_current_id()];
}

task_t *proc_set_current(task_t *next)
{
    unsigned cpu = cpu_current_id();
    task_t *old = g_cpu_current[cpu];
    /* Publish the outgoing task before replacing current. Reapers must keep
     * its task storage and kernel stack alive until the switch completes. */
    __atomic_store_n(&g_cpu_switching_out[cpu], old, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu_current[cpu], next, __ATOMIC_RELEASE);
    return old;
}

void proc_switch_complete(void)
{
    unsigned cpu = cpu_current_id();
    task_t *old = __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE);
    int reap = old && __atomic_load_n(&old->state, __ATOMIC_ACQUIRE) == PROC_ZOMBIE;

    __atomic_store_n(&g_cpu_switching_out[cpu], NULL, __ATOMIC_RELEASE);
    if (reap)
        proc_sched_note_zombie();
}

task_t *proc_current_on_cpu(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return NULL;
    return __atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE);
}

int proc_task_is_current_any_cpu(task_t *task)
{
    if (!task)
        return 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (proc_current_on_cpu(cpu) == task ||
            __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE) == task)
            return 1;
    }
    return 0;
}
