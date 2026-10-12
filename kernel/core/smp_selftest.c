#if defined(BRINGUP) && !defined(CONFIG_MCU)
#include "core/smp_test.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/lifetime.h"
#include "proc/proc.h"

static unsigned timer_irqs[CONFIG_NR_CPUS];
static unsigned phase, finished, remote_cpu;
static task_t *worker_task;
static int worker_pid;
static uintptr_t stack_address;
static uint32_t *allocation;

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c))                                                              \
            panic("SMP selftest: " #c);                                        \
    } while (0)

void smp_selftest_note_timer_irq(void) {
    __atomic_fetch_add(&timer_irqs[cpu_current_id()], 1, __ATOMIC_RELAXED);
}

static int set_cpu(task_t *task, unsigned cpu) {
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = 1U << cpu,
    };
    return proc_sched_set(task, &config);
}

static void worker(void) {
    CHECK(cpu_current_id() == remote_cpu);
    CHECK(proc_current()->pid == worker_pid);
    volatile uint32_t stack_words[4] = {0xa201, 0xa202, 0xa203, 0xa204};
    stack_address = (uintptr_t)&stack_words[0];
    uint32_t *p = kmalloc(64);
    CHECK(p != NULL);
    p[0] = 0xa20c1234;
    p[15] = 0x1234c20a;
    allocation = p;
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
    CHECK(token.seq != 0);
    __atomic_store_n(&phase, 1, __ATOMIC_RELEASE);
    CHECK(proc_park_commit(token) == PROC_WAKE_EVENT);
    proc_park_finish(token);
    CHECK(cpu_current_id() == 0);
    CHECK(proc_current()->pid == worker_pid);
    CHECK(allocation == p && p[0] == 0x5a20abcd && p[15] == 0x1234c20a);
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(30);
    proc_sleep_until(deadline);
    CHECK(timer_get_ticks() >= deadline);
    CHECK((uintptr_t)&stack_words[0] == stack_address);
    CHECK(stack_words[0] == 0xa201 && stack_words[3] == 0xa204);
    kfree(p);
    proc_exit(0);
}

static void controller(void) {
    CHECK(cpu_current_id() == 0);
    worker_pid = proc_alloc_deferred(worker);
    CHECK(worker_pid > 0);
    worker_task = proc_find_get(worker_pid);
    CHECK(worker_task && set_cpu(worker_task, remote_cpu) == 0);
    proc_publish_deferred_task(worker_task);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    for (;;) {
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == 1) {
            uint64_t flags = spin_lock_irqsave(&worker_task->park_lock);
            int parked =
                worker_task->state == PROC_BLOCKED && !worker_task->on_cpu;
            spin_unlock_irqrestore(&worker_task->park_lock, flags);
            if (parked)
                break;
        }
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(allocation[0] == 0xa20c1234);
    allocation[0] = 0x5a20abcd;
    CHECK(set_cpu(worker_task, 0) == 0);
    CHECK(smp_membarrier_sync_all() ==
          0); /* Actual remote IPI acknowledgments. */
    CHECK(proc_try_wake(worker_task, worker_task->wait_seq, PROC_WAKE_EVENT));
    int status = -1;
    CHECK(proc_wait4(worker_pid, &status, 0) == worker_pid && status == 0);
    proc_put(worker_task);
    /* Every online CPU must have taken a real timer IRQ, not just read its
     * free-running counter or advanced a scheduler's polling clock. */
    uint32_t mask = smp_online_cpu_mask();
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        while ((mask & (1U << cpu)) &&
               !__atomic_load_n(&timer_irqs[cpu], __ATOMIC_ACQUIRE)) {
            CHECK(timer_get_ticks() < limit);
            proc_yield();
        }
    }
    printf(
        "[SMP-TEST] PASS cpus=%u mask=0x%x pid=%d migrated=%u->0 "
        "stack=0x%lx heap=cross-cpu timer-irqs=all-online ipi=acked exit=0\n",
        smp_online_cpu_count(), mask, worker_pid, remote_cpu,
        (unsigned long)stack_address);
    __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}

void smp_selftest_boot(void) {
    const char *args = bootargs_get();
#ifdef CONFIG_SMP_SELFTEST
    int enabled = 1;
#else
    int enabled = 0;
#endif
    /* Match a complete bootarg token, so incidental substring text cannot
     * turn a normal bring-up into a scheduling test. */
    while (args && *args) {
        while (*args == ' ')
            args++;
        size_t n = 0;
        while (args[n] && args[n] != ' ')
            n++;
        if (n == sizeof("a20.smp_test=1") - 1 &&
            memcmp(args, "a20.smp_test=1", n) == 0)
            enabled = 1;
        args += n;
    }
    if (!enabled)
        return;
    CHECK(smp_online_cpu_count() >= 2 && cpu_current_id() == 0);
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        if (smp_cpu_is_online(cpu))
            remote_cpu = cpu;
    CHECK(remote_cpu > 0);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        const smp_cpu_desc_t *desc = smp_cpu_desc(cpu);
        if (desc && smp_cpu_is_online(cpu))
            printf("[SMP-TEST] cpu=%u hw=%lu\n", cpu,
                   (unsigned long)desc->hw_id);
    }
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    int saved = arch_irqs_enabled();
    arch_local_irq_disable();
    int pid = proc_alloc_deferred(controller);
    task_t *task = proc_find_get(pid);
    CHECK(pid > 0 && task && set_cpu(task, 0) == 0);
    proc_publish_deferred_task(task);
    proc_put(task);
    arch_local_irq_enable();
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(10000);
    while (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    /* Parent is the boot idle context; let normal scheduler retirement clean
     * the controller rather than destroying an on-CPU task from the host. */
    do {
        proc_yield();
        proc_lifetime_snapshot(&after);
        CHECK(timer_get_ticks() < limit);
    } while (after.task_objects != before.task_objects ||
             after.pid_entries != before.pid_entries ||
             after.listed_tasks != before.listed_tasks);
    CHECK(after.lifetime_errors == before.lifetime_errors &&
          after.scheduler_violations == before.scheduler_violations &&
          after.state_violations == before.state_violations);
    printf("[SMP-TEST] CLEANUP PASS task-objects=%lu pids=%lu listed=%lu\n",
           after.task_objects, after.pid_entries, after.listed_tasks);
    arch_local_irq_disable();
    if (saved)
        arch_local_irq_enable();
}
#endif
