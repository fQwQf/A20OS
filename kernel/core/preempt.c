#ifdef CONFIG_KERNEL_PREEMPT

#include "core/preempt.h"
#include "core/consts.h"
#include "core/cpu.h"

/*
 * Why a per-CPU counter is sound here, with no save/restore in the task
 * switch:
 *
 * A context switch is only ever taken when preempt_count() == 0, and that is
 * enforced in two independent places rather than assumed:
 *
 *   - a voluntary switch (sched()) is only legal with no lock held -- the
 *     lock contract in core/lock.h bans blocking while holding a spinlock, and
 *     context_switch_locked() panics if it is ever violated, so the assert
 *     covers the violation rather than the caller;
 *   - a forced switch at the IRQ return point requires preempt_allowed(),
 *     which is exactly "preempt == 0 && hardirq == 0".  An interrupt taken
 *     inside a spinlock section leaves preempt > 0, so the decision point
 *     declines; an interrupt taken with preempt == 0 either completes and
 *     leaves the counter at 0, or reaches a lock acquire that raises it
 *     before the handler returns.
 *
 * So the value is zero at every __switch, on every path, and the per-CPU slot
 * needs no per-task save/restore -- which is what makes the whole mechanism
 * need no new assembly on any architecture.
 *
 * The counter is per-CPU rather than per-task precisely because it is only
 * ever read while it is provably zero across a switch; a per-task counter
 * would have to be saved and restored by __switch, which is per-architecture
 * assembly that this design deliberately avoids touching.
 */

/* One line per CPU, same discipline as g_cpu_ticks in kernel/proc/sched.c:
 * this is written from the IRQ return path on every CPU, so the slots must not
 * share a line. */
static cpu_preempt_state_t g_preempt_state[CONFIG_NR_CPUS];

/*
 * cpu_current_id() is already the clamped wrapper (core/cpu.h returns 0 for an
 * out-of-range id), so this only spells out the one case the wrapper cannot
 * cover: a build where CONFIG_NR_CPUS is smaller than what the arch backend
 * reports, during early boot before the slot array exists for it.
 */
static inline cpu_preempt_state_t *preempt_slot(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu >= CONFIG_NR_CPUS)
        cpu = 0;
    return &g_preempt_state[cpu];
}

void preempt_state_init(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return;
    __atomic_store_n(&g_preempt_state[cpu].preempt, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_preempt_state[cpu].hardirq, 0, __ATOMIC_RELAXED);
}

/* ACQUIRE on the way in so that everything the critical section protects is
 * ordered after the counter bump that made it non-preemptible. */
void preempt_disable(void)
{
    /* Pin the task to this CPU while selecting and updating its per-CPU slot.
     * Otherwise an IRQ could arrive after cpu_current_id() but before the
     * increment, switch the still-preemptible task to another CPU, and leave
     * this CPU's counter raised while the task holds a lock elsewhere. */
    uint64_t irq_flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    __atomic_fetch_add(&preempt_slot()->preempt, 1, __ATOMIC_ACQUIRE);
    if (irq_flags)
        arch_local_irq_enable();
}

/* RELEASE on the way out, paired with the acquire above: a task that resumes
 * after this decrement must not be preempted again before it has observed the
 * state the critical section left behind. */
void preempt_enable(void)
{
    __atomic_fetch_sub(&preempt_slot()->preempt, 1, __ATOMIC_RELEASE);
}

unsigned preempt_count(void)
{
    return __atomic_load_n(&preempt_slot()->preempt, __ATOMIC_ACQUIRE);
}

int preempt_allowed(void)
{
    cpu_preempt_state_t *s = preempt_slot();
    unsigned preempt = __atomic_load_n(&s->preempt, __ATOMIC_ACQUIRE);
    unsigned hardirq = __atomic_load_n(&s->hardirq, __ATOMIC_ACQUIRE);
    return preempt == 0 && hardirq == 0;
}

void hardirq_enter(void)
{
    __atomic_fetch_add(&preempt_slot()->hardirq, 1, __ATOMIC_ACQUIRE);
}

void hardirq_exit(void)
{
    __atomic_fetch_sub(&preempt_slot()->hardirq, 1, __ATOMIC_RELEASE);
}

int in_hardirq(void)
{
    return __atomic_load_n(&preempt_slot()->hardirq, __ATOMIC_ACQUIRE) != 0;
}

#endif /* CONFIG_KERNEL_PREEMPT */
