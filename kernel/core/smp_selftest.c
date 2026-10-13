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
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "proc/lifetime.h"
#include "proc/proc.h"

static int disk_test;
static unsigned timer_irqs[CONFIG_NR_CPUS];
static unsigned phase, finished, remote_cpu;
static task_t *worker_task;
static int worker_pid;
static uintptr_t stack_address;
static uint32_t *allocation;
static uint64_t clock_before, clock_remote, remote_deadline;
static unsigned clock_sampled, clock_checked;

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

/* Opt-in scratch disk only. Validate the host fixture before any writes.
 * Direct class operations bypass the filesystem/page cache, exercising DMA,
 * transport notification and completion on the real device owner. */
static void disk_roundtrip(void) {
    device_t *dev = device_find_by_class(DEV_CLASS_BLOCK, 0);
    CHECK(dev && dev->drv && dev->drv->class_ops);
    const block_dev_ops_t *ops = dev->drv->class_ops;
    CHECK(ops->read && ops->write && ops->flush && ops->capacity && ops->sector_size);
    CHECK(ops->capacity(dev) == 4096 && ops->sector_size(dev) == 512);
    unsigned char *buffer = kmalloc(512);
    CHECK(buffer);
    CHECK(ops->read(dev, 7, buffer, 1) == 0);
    for (unsigned i = 0; i < 512; i++) CHECK(buffer[i] == (unsigned char)(i * 17 + 0xa2));
    for (unsigned round = 0; round < 32; round++) {
        for (unsigned i = 0; i < 512; i++) buffer[i] = (unsigned char)(i * 29 + round * 7 + 0x20);
        CHECK(ops->write(dev, 8 + round, buffer, 1) == 0);
        CHECK(ops->flush(dev) == 0);
        memset(buffer, 0, 512);
        CHECK(ops->read(dev, 8 + round, buffer, 1) == 0);
        for (unsigned i = 0; i < 512; i++)
            CHECK(buffer[i] == (unsigned char)(i * 29 + round * 7 + 0x20));
        CHECK(cpu_current_id() == remote_cpu);
    }
    kfree(buffer);
    printf("[SMP-DISK] PASS cpu=%u sectors=32 bytes=16384 flush=32 readback=32\n", cpu_current_id());
}

static void worker(void) {
    CHECK(cpu_current_id() == remote_cpu);
    clock_remote = timer_get_ticks();
    __atomic_store_n(&clock_sampled, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&clock_checked, __ATOMIC_ACQUIRE))
        proc_yield();
    /* Deadline was formed on CPU 0, but expires on the remote CPU. */
    proc_sleep_until(remote_deadline);
    CHECK(cpu_current_id() == remote_cpu);
    CHECK(timer_get_ticks() >= remote_deadline);
    CHECK(proc_current()->pid == worker_pid);
    if (disk_test) disk_roundtrip();
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
    clock_before = timer_get_ticks();
    proc_publish_deferred_task(worker_task);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!__atomic_load_n(&clock_sampled, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    uint64_t clock_after = timer_get_ticks();
    printf("[SMP-TEST] clock before=%lu remote=%lu after=%lu\n",
           (unsigned long)clock_before, (unsigned long)clock_remote,
           (unsigned long)clock_after);
#ifdef CONFIG_QEMU_SHARED_SMP
    /* This backend promises one published clock. Ordinary platform counters
     * have their own synchronization bounds; record the sample there without
     * imposing this new strict cross-CPU ordering contract on them. */
    CHECK(clock_remote >= clock_before && clock_remote <= clock_after);
#endif
    remote_deadline = clock_after + MS_TO_TICKS(30);
    __atomic_store_n(&clock_checked, 1, __ATOMIC_RELEASE);
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
        "stack=0x%lx heap=cross-cpu timer-irqs=all-online ipi=acked exit=0 "
#ifdef CONFIG_QEMU_SHARED_SMP
        "clock=ordered sleep=remote\n",
#else
        "clock=sampled sleep=remote\n",
#endif
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
        if (n == sizeof("a20.smp_disk_test=1") - 1 &&
            memcmp(args, "a20.smp_disk_test=1", n) == 0)
            disk_test = 1;
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
