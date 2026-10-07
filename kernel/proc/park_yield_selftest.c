#include "proc/proc_internal.h"
#include "proc/park.h"

#include "core/cpu.h"
#include "core/lock.h"

#ifdef CONFIG_RISCV64

/* This bootarg-only probe runs on one CPU. The helper is deliberately runnable
 * while the test task owns a PREPARING park token, so proc_yield() must really
 * switch away and the switch-completion path must put the test task back. */
static task_t *park_yield_test_task;
static uint64_t park_yield_test_seq;
static unsigned park_yield_test_saw_ready;
static unsigned park_yield_test_woke;

static void park_yield_test_helper(void) {
    for (;;) {
        task_t *task = __atomic_load_n(&park_yield_test_task, __ATOMIC_ACQUIRE);
        uint64_t seq = __atomic_load_n(&park_yield_test_seq, __ATOMIC_ACQUIRE);
        if (!task || !seq) {
            proc_yield();
            continue;
        }

        uint64_t flags = spin_lock_irqsave(&task->park_lock);
        proc_park_state_t park = task->park_state;
        proc_state_t state = task->state;
        uint64_t task_seq = task->wait_seq;
        int on_cpu = task->on_cpu;
        int on_rq = __atomic_load_n(&task->on_rq, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&task->park_lock, flags);

        if (task_seq != seq) {
            proc_yield();
            continue;
        }
        if (park == PROC_PARK_PREPARING) {
            /* The test only passes if the helper observes a real switch-out
             * while the original token remains live and the task is queued. */
            if (state == PROC_READY && !on_cpu && on_rq)
                __atomic_store_n(&park_yield_test_saw_ready, 1,
                                 __ATOMIC_RELEASE);
            proc_yield();
            continue;
        }
        if (park == PROC_PARK_PARKED && state == PROC_BLOCKED) {
            /* Keep the single CPU from running the parent between publishing
             * the wake and recording that this helper issued it. */
            int irqs_were_enabled = arch_irqs_enabled();
            arch_local_irq_disable();
            int woke = proc_try_wake(task, seq, PROC_WAKE_EVENT);
            if (woke)
                __atomic_store_n(&park_yield_test_woke, 1, __ATOMIC_RELEASE);
            if (irqs_were_enabled)
                arch_local_irq_enable();
            if (woke)
                return;
        }
        proc_yield();
    }
}

int riscv64_sched_park_yield_selftest(void) {
    task_t *self = proc_current();
    if (!self || self == proc_idle_task() || CONFIG_NR_CPUS != 1 ||
        cpu_current_id() != 0)
        return -1;

    __atomic_store_n(&park_yield_test_task, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&park_yield_test_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&park_yield_test_saw_ready, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&park_yield_test_woke, 0, __ATOMIC_RELAXED);

    int helper_pid = proc_alloc(park_yield_test_helper);
    if (helper_pid < 0)
        return -1;

    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    if (token.task != self || token.prepare_error != PROC_PARK_PREPARE_OK)
        return -1;
    __atomic_store_n(&park_yield_test_task, self, __ATOMIC_RELEASE);
    __atomic_store_n(&park_yield_test_seq, token.seq, __ATOMIC_RELEASE);

    /* Explicitly yield after PREPARING is published. A timer interrupt may
     * cause an earlier yield too; the helper's READY+PREPARING observation
     * covers either route, while this call makes the regression deterministic. */
    proc_yield();

    uint64_t flags = spin_lock_irqsave(&self->park_lock);
    int resumed =
        proc_current() == self && self->state == PROC_RUNNING && self->on_cpu &&
        __atomic_load_n(&self->owner_cpu, __ATOMIC_RELAXED) ==
            cpu_current_id() &&
        self->park_state == PROC_PARK_PREPARING && self->wait_seq == token.seq;
    spin_unlock_irqrestore(&self->park_lock, flags);
    if (!resumed ||
        !__atomic_load_n(&park_yield_test_saw_ready, __ATOMIC_ACQUIRE))
        return -1;

    proc_wake_reason_t reason = proc_park_commit(token);
    if (reason != PROC_WAKE_EVENT ||
        !__atomic_load_n(&park_yield_test_woke, __ATOMIC_ACQUIRE))
        return -1;
    proc_park_finish(token);

    flags = spin_lock_irqsave(&self->park_lock);
    int finished =
        self->park_state == PROC_PARK_IDLE && self->wait_seq == token.seq;
    spin_unlock_irqrestore(&self->park_lock, flags);
    if (!finished)
        return -1;

    int status = -1;
    if (proc_wait4(helper_pid, &status, 0) != helper_pid || status != 0)
        return -1;
    return 0;
}

#endif /* CONFIG_RISCV64 */
