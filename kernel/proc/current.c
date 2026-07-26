#include "proc/proc_internal.h"
#include "core/cpu.h"
#include "core/panic.h"

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
 * - g_cpu_switching_out keeps the old stack owner visible until the replacement
 *   task calls proc_switch_complete(). That completion is also the only path
 *   which clears old->on_cpu and publishes a raced READY task to a runqueue.
 * - Cross-CPU wakeup and IPI reschedule tests must prove that cpu_current_id(),
 *   runqueue selection, and current slot lookup agree for the CPU handling the
 *   interrupt.
 */
static task_t *g_cpu_current[CONFIG_NR_CPUS];
static task_t *g_cpu_switching_out[CONFIG_NR_CPUS];

/* Caller holds proc_lock whenever a pending outgoing task can exist. */
static int proc_switch_complete_locked(unsigned cpu)
{
    task_t *old =
        __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE);
    if (!old)
        return 0;

#if CONFIG_DEBUG_SCHED_STATE
    if (!old->on_cpu || old->owner_cpu != cpu)
        panic("switch complete: pid=%d on_cpu=%d owner=%u cpu=%u",
              old->pid, old->on_cpu, old->owner_cpu, cpu);
#endif
    old->on_cpu = 0;
    old->owner_cpu = PROC_CPU_NONE;

    /*
     * yield and Park/Wake may publish READY while the old task still owns this
     * CPU. It becomes queueable only after execution has continued on the
     * replacement stack.
     */
    if (old->state == PROC_READY && !old->dispatching && !old->on_rq)
        proc_runq_enqueue_locked(old);
    proc_sched_assert_task_locked(old);

    __atomic_store_n(&g_cpu_switching_out[cpu], NULL, __ATOMIC_RELEASE);
    return old->state == PROC_ZOMBIE;
}

task_t *proc_current(void)
{
    return g_cpu_current[cpu_current_id()];
}

task_t *proc_set_current(task_t *next)
{
    unsigned cpu = cpu_current_id();
    /*
     * Some architectures restore interrupt state in __switch before returning
     * to the C completion hook. If that incoming task is immediately
     * preempted, finish the already-inactive predecessor before replacing the
     * single switching_out slot.
     */
    if (__atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE) &&
        proc_switch_complete_locked(cpu))
        proc_sched_note_zombie();

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

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int reap = proc_switch_complete_locked(cpu);
    spin_unlock_irqrestore(&proc_lock, flags);
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
