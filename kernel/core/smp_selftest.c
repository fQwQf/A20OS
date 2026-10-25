#if defined(BRINGUP) && !defined(CONFIG_MCU)
#include "core/smp_test.h"
#include "core/bootargs.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/sync.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "proc/lifetime.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"

static int disk_test;
static unsigned timer_irqs[CONFIG_NR_CPUS];
static unsigned phase, finished, remote_cpu;
static unsigned worker_park_cpu;
static task_t *worker_task;
static int worker_pid;
static uintptr_t stack_address;
static uint32_t *allocation;
static uint64_t clock_before, clock_remote, remote_deadline;
static unsigned clock_sampled, clock_checked;
#if defined(CONFIG_WAIT_TIMER_HEAP_TEST_ARRAY_BOUNDS)
#define SMP_WAIT_TIMER_CAPACITY_TEST 1
static task_t *race_waiter_task;
static int race_waiter_pid, race_waker_pid;
static uint64_t race_wait_seq, race_deadline;
static unsigned race_waiter_phase, race_hook_stage;
static unsigned race_waiter_cpu, race_waker_cpu, race_expirer_cpu;
static int race_wake_result, race_timeout_result;
static proc_wake_reason_t race_wait_reason;
#else
#define SMP_WAIT_TIMER_CAPACITY_TEST 0
#endif
static wait_queue_t wait_queue_test = WAIT_QUEUE_INIT;
static wait_queue_entry_t wait_queue_test_entry;
static unsigned wait_queue_test_phase;
static rw_mutex_t rw_mutex_fairness_test = RW_MUTEX_INIT;
static task_t *rw_mutex_writer_task;
static task_t *rw_mutex_reader_task;
static int rw_mutex_writer_pid, rw_mutex_reader_pid;
static unsigned rw_mutex_writer_started, rw_mutex_writer_acquired;
static unsigned rw_mutex_writer_release, rw_mutex_reader_started;
static unsigned rw_mutex_reader_acquired;
static mutex_t mutex_owner_test = MUTEX_INIT;
static task_t *mutex_waiter_task;
static int mutex_waiter_pid;
static unsigned mutex_waiter_started, mutex_waiter_acquired;
#if defined(CONFIG_RV32_SYNC_TEST)
#define SYNC_TEST_CPU 0U
static completion_t rv32_completion_test = COMPLETION_INIT;
static task_t *rv32_completion_task;
static int rv32_completion_pid;
static unsigned rv32_completion_started, rv32_completion_returned;
static unsigned rv32_sync_test_finished;
#else
#define SYNC_TEST_CPU 1U
#endif

#define WAIT_QUEUE_TEST_KEY ((uintptr_t)0xa20)

#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c))                                                              \
            panic("SMP selftest: " #c);                                        \
    } while (0)

void smp_selftest_note_timer_irq(void) {
    __atomic_fetch_add(&timer_irqs[cpu_current_id()], 1, __ATOMIC_RELAXED);
}

/* The timer module calls this after dropping its heap lock and before trying
 * park_lock. A focused race profile replaces the no-op below with a two-CPU
 * rendezvous; keeping the symbol in every SMP selftest build allows the Rust
 * and C timer implementations to share the same fixture. */
void smp_test_wait_timer_after_dequeue(task_t *task, uint64_t seq)
{
#if SMP_WAIT_TIMER_CAPACITY_TEST
    if (task != race_waiter_task || seq != race_wait_seq)
        return;
    CHECK(cpu_current_id() == 0);
    race_expirer_cpu = cpu_current_id();
    __atomic_store_n(&race_hook_stage, 1, __ATOMIC_RELEASE);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (__atomic_load_n(&race_hook_stage, __ATOMIC_ACQUIRE) != 2)
        CHECK(timer_get_ticks() < limit);
#else
    (void)task;
    (void)seq;
#endif
}

void smp_test_wait_timer_after_fire(task_t *task, uint64_t seq, int result)
{
#if SMP_WAIT_TIMER_CAPACITY_TEST
    if (task == race_waiter_task && seq == race_wait_seq)
        __atomic_store_n(&race_timeout_result, result, __ATOMIC_RELEASE);
#else
    (void)task;
    (void)seq;
    (void)result;
#endif
}

static int set_cpu(task_t *task, unsigned cpu) {
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = 1U << cpu,
    };
    return proc_sched_set(task, &config);
}

static void rw_mutex_fairness_writer(void)
{
    CHECK(cpu_current_id() == SYNC_TEST_CPU);
    __atomic_store_n(&rw_mutex_writer_started, 1, __ATOMIC_RELEASE);
    rw_mutex_write_lock(&rw_mutex_fairness_test);
    __atomic_store_n(&rw_mutex_writer_acquired, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&rw_mutex_writer_release, __ATOMIC_ACQUIRE))
        proc_yield();
    rw_mutex_write_unlock(&rw_mutex_fairness_test);
    proc_exit(0);
}

static void rw_mutex_fairness_reader(void)
{
    CHECK(cpu_current_id() == SYNC_TEST_CPU);
    __atomic_store_n(&rw_mutex_reader_started, 1, __ATOMIC_RELEASE);
    rw_mutex_read_lock(&rw_mutex_fairness_test);
    __atomic_store_n(&rw_mutex_reader_acquired, 1, __ATOMIC_RELEASE);
    rw_mutex_read_unlock(&rw_mutex_fairness_test);
    proc_exit(0);
}

static void mutex_owner_waiter(void)
{
    CHECK(cpu_current_id() == SYNC_TEST_CPU);
    __atomic_store_n(&mutex_waiter_started, 1, __ATOMIC_RELEASE);
    mutex_lock(&mutex_owner_test);
    __atomic_store_n(&mutex_waiter_acquired, 1, __ATOMIC_RELEASE);
    mutex_unlock(&mutex_owner_test);
    proc_exit(0);
}

static bool task_is_fully_parked(task_t *task)
{
    uint64_t flags = spin_lock_irqsave(&task->park_lock);
    bool parked = task->state == PROC_BLOCKED && !task->on_cpu &&
                  task->park_state == PROC_PARK_PARKED;
    spin_unlock_irqrestore(&task->park_lock, flags);
    return parked;
}

static void run_rw_mutex_writer_preference_test(void)
{
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    rw_mutex_init(&rw_mutex_fairness_test);
    rw_mutex_writer_started = 0;
    rw_mutex_writer_acquired = 0;
    rw_mutex_writer_release = 0;
    rw_mutex_reader_started = 0;
    rw_mutex_reader_acquired = 0;

    rw_mutex_read_lock(&rw_mutex_fairness_test);
    rw_mutex_writer_pid = proc_alloc_deferred(rw_mutex_fairness_writer);
    CHECK(rw_mutex_writer_pid > 0);
    rw_mutex_writer_task = proc_find_get(rw_mutex_writer_pid);
    CHECK(rw_mutex_writer_task && set_cpu(rw_mutex_writer_task, SYNC_TEST_CPU) == 0);
    proc_publish_deferred_task(rw_mutex_writer_task);

    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!task_is_fully_parked(rw_mutex_writer_task)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(__atomic_load_n(&rw_mutex_writer_started, __ATOMIC_ACQUIRE));

    rw_mutex_reader_pid = proc_alloc_deferred(rw_mutex_fairness_reader);
    CHECK(rw_mutex_reader_pid > 0);
    rw_mutex_reader_task = proc_find_get(rw_mutex_reader_pid);
    CHECK(rw_mutex_reader_task && set_cpu(rw_mutex_reader_task, SYNC_TEST_CPU) == 0);
    proc_publish_deferred_task(rw_mutex_reader_task);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!task_is_fully_parked(rw_mutex_reader_task)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(__atomic_load_n(&rw_mutex_reader_started, __ATOMIC_ACQUIRE));
    CHECK(!__atomic_load_n(&rw_mutex_writer_acquired, __ATOMIC_ACQUIRE) &&
          !__atomic_load_n(&rw_mutex_reader_acquired, __ATOMIC_ACQUIRE));

    rw_mutex_read_unlock(&rw_mutex_fairness_test);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!__atomic_load_n(&rw_mutex_writer_acquired, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(!__atomic_load_n(&rw_mutex_reader_acquired, __ATOMIC_ACQUIRE));
    __atomic_store_n(&rw_mutex_writer_release, 1, __ATOMIC_RELEASE);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!__atomic_load_n(&rw_mutex_reader_acquired, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }

    int status = -1;
    CHECK(proc_wait4(rw_mutex_writer_pid, &status, 0) == rw_mutex_writer_pid &&
          status == 0);
    CHECK(proc_wait4(rw_mutex_reader_pid, &status, 0) == rw_mutex_reader_pid &&
          status == 0);
    proc_put(rw_mutex_writer_task);
    proc_put(rw_mutex_reader_task);
    rw_mutex_writer_task = NULL;
    rw_mutex_reader_task = NULL;
    proc_lifetime_snapshot(&after);
    CHECK(after.task_refs == before.task_refs);
    printf("[RW_MUTEX_WRITER_PREFERENCE: PASS writer_first=1 blocked_reader=1 cpu=%u refs=stable]\n", SYNC_TEST_CPU);
}

static void run_mutex_owner_wait_test(void)
{
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    mutex_init(&mutex_owner_test);
    mutex_waiter_started = 0;
    mutex_waiter_acquired = 0;

    /* The existing recursive depth workaround is part of mutex state too. */
    mutex_lock(&mutex_owner_test);
    mutex_lock(&mutex_owner_test);
    uint64_t flags = spin_lock_irqsave(&mutex_owner_test.lock);
    CHECK(mutex_owner_test.locked && mutex_owner_test.depth == 2 &&
          mutex_owner_test.owner == proc_current());
    spin_unlock_irqrestore(&mutex_owner_test.lock, flags);

    mutex_waiter_pid = proc_alloc_deferred(mutex_owner_waiter);
    CHECK(mutex_waiter_pid > 0);
    mutex_waiter_task = proc_find_get(mutex_waiter_pid);
    CHECK(mutex_waiter_task && set_cpu(mutex_waiter_task, SYNC_TEST_CPU) == 0);
    proc_publish_deferred_task(mutex_waiter_task);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!task_is_fully_parked(mutex_waiter_task)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(__atomic_load_n(&mutex_waiter_started, __ATOMIC_ACQUIRE));
    CHECK(!__atomic_load_n(&mutex_waiter_acquired, __ATOMIC_ACQUIRE));
    mutex_unlock(&mutex_owner_test);
    flags = spin_lock_irqsave(&mutex_owner_test.lock);
    CHECK(mutex_owner_test.locked && mutex_owner_test.depth == 1);
    spin_unlock_irqrestore(&mutex_owner_test.lock, flags);
    mutex_unlock(&mutex_owner_test);

    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!__atomic_load_n(&mutex_waiter_acquired, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    int status = -1;
    CHECK(proc_wait4(mutex_waiter_pid, &status, 0) == mutex_waiter_pid &&
          status == 0);
    proc_put(mutex_waiter_task);
    mutex_waiter_task = NULL;
    flags = spin_lock_irqsave(&mutex_owner_test.lock);
    CHECK(!mutex_owner_test.locked && mutex_owner_test.depth == 0 &&
          mutex_owner_test.owner == NULL);
    spin_unlock_irqrestore(&mutex_owner_test.lock, flags);
    proc_lifetime_snapshot(&after);
    CHECK(after.task_refs == before.task_refs);
    printf("[MUTEX_OWNER_WAIT: PASS recursive_depth=2 waiter_cpu=%u wake=1 refs=stable]\n", SYNC_TEST_CPU);
}

#if defined(CONFIG_RV32_SYNC_TEST)
static void rv32_completion_waiter(void)
{
    __atomic_store_n(&rv32_completion_started, 1, __ATOMIC_RELEASE);
    wait_for_completion(&rv32_completion_test);
    __atomic_store_n(&rv32_completion_returned, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}

static void run_rv32_completion_test(void)
{
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    completion_init(&rv32_completion_test);
    rv32_completion_started = 0;
    rv32_completion_returned = 0;
    rv32_completion_pid = proc_alloc_deferred(rv32_completion_waiter);
    CHECK(rv32_completion_pid > 0);
    rv32_completion_task = proc_find_get(rv32_completion_pid);
    CHECK(rv32_completion_task && set_cpu(rv32_completion_task, 0) == 0);
    proc_publish_deferred_task(rv32_completion_task);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (!task_is_fully_parked(rv32_completion_task)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(__atomic_load_n(&rv32_completion_started, __ATOMIC_ACQUIRE));
    CHECK(!__atomic_load_n(&rv32_completion_returned, __ATOMIC_ACQUIRE));
    uint64_t flags = spin_lock_irqsave(&rv32_completion_test.waiters.lock);
    CHECK(rv32_completion_test.waiters.head != NULL &&
          rv32_completion_test.waiters.head->task == rv32_completion_task &&
          rv32_completion_test.waiters.head->linked);
    spin_unlock_irqrestore(&rv32_completion_test.waiters.lock, flags);
    proc_lifetime_stats_t blocked;
    proc_lifetime_snapshot(&blocked);
    CHECK(blocked.wait_entries > before.wait_entries);
    complete(&rv32_completion_test);
    while (!__atomic_load_n(&rv32_completion_returned, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    int status = -1;
    CHECK(proc_wait4(rv32_completion_pid, &status, 0) == rv32_completion_pid &&
          status == 0);
    proc_put(rv32_completion_task);
    rv32_completion_task = NULL;
    proc_lifetime_snapshot(&after);
    CHECK(after.task_refs == before.task_refs);
    CHECK(after.wait_entries == before.wait_entries);
    printf("[RV32_COMPLETION_BLOCK_WAKE: PASS cpu=0 blocked=1 wake=1 refs=stable]\n");
}

static void rv32_sync_test_controller(void)
{
    CHECK(cpu_current_id() == 0);
    run_rv32_completion_test();
    run_mutex_owner_wait_test();
    run_rw_mutex_writer_preference_test();
    __atomic_store_n(&rv32_sync_test_finished, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}
#endif

/* Opt-in scratch disk only. Validate the host fixture before any writes.
 * Direct class operations bypass the filesystem/page cache, exercising DMA,
 * transport notification and completion on the real device owner. */
static void disk_roundtrip(void) {
    device_t *dev = device_find_get_by_class(DEV_CLASS_BLOCK, 0);
    CHECK(dev);
    class_device_t *cdev = class_device_ref_for_device(dev);
    const void *table;
    CHECK(class_device_ops_begin(cdev, DEV_CLASS_BLOCK, &table) == 0);
    const block_dev_ops_t *ops = table;
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
    class_device_call_end(cdev);
    class_device_put(cdev);
    device_put(dev);
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
    /* Keep one real wait-timer heap entry while parked.  The controller uses
     * this slot to force a second prepare through the timer-capacity failure
     * path and then wakes this waiter from the other CPU. */
    uint64_t park_deadline = timer_get_ticks() + MS_TO_TICKS(60000);
    worker_park_cpu = cpu_current_id();
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, park_deadline);
    CHECK(token.seq != 0);
    CHECK(token.prepare_error == PROC_PARK_PREPARE_OK);
    CHECK(wait_queue_link(&wait_queue_test, &wait_queue_test_entry, token,
                          WAIT_QUEUE_TEST_KEY));
    __atomic_store_n(&phase, 1, __ATOMIC_RELEASE);
    CHECK(proc_park_commit(token) == PROC_WAKE_EVENT);
    proc_park_finish(token);
    wait_queue_unlink(&wait_queue_test, &wait_queue_test_entry);
    __atomic_store_n(&wait_queue_test_phase, 2, __ATOMIC_RELEASE);
    CHECK(cpu_current_id() == remote_cpu);
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

#if SMP_WAIT_TIMER_CAPACITY_TEST
static void wait_timer_race_waiter(void)
{
    CHECK(cpu_current_id() == 1);
    race_waiter_cpu = cpu_current_id();
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, race_deadline);
    CHECK(token.prepare_error == PROC_PARK_PREPARE_OK && token.seq != 0);
    race_wait_seq = token.seq;
    __atomic_store_n(&race_waiter_phase, 1, __ATOMIC_RELEASE);
    race_wait_reason = proc_park_commit(token);
    proc_park_finish(token);
    CHECK(race_wait_reason == PROC_WAKE_EVENT);
    __atomic_store_n(&race_waiter_phase, 2, __ATOMIC_RELEASE);
    proc_exit(0);
}

static void wait_timer_race_waker(void)
{
    CHECK(cpu_current_id() == 1);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (__atomic_load_n(&race_hook_stage, __ATOMIC_ACQUIRE) != 1) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    race_waker_cpu = cpu_current_id();
    race_wake_result = proc_try_wake(
        race_waiter_task, race_wait_seq, PROC_WAKE_EVENT);
    CHECK(race_wake_result == 1);
    __atomic_store_n(&race_hook_stage, 2, __ATOMIC_RELEASE);
    proc_exit(0);
}

static void run_wait_timer_cancel_expire_race(void)
{
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    race_deadline = timer_get_ticks() + MS_TO_TICKS(60000);
    race_waiter_pid = proc_alloc_deferred(wait_timer_race_waiter);
    race_waker_pid = proc_alloc_deferred(wait_timer_race_waker);
    CHECK(race_waiter_pid > 0 && race_waker_pid > 0);
    race_waiter_task = proc_find_get(race_waiter_pid);
    task_t *waker = proc_find_get(race_waker_pid);
    CHECK(race_waiter_task && waker);
    CHECK(set_cpu(race_waiter_task, 1) == 0 && set_cpu(waker, 1) == 0);
    race_wait_seq = 0;
    race_waiter_phase = 0;
    race_hook_stage = 0;
    race_wake_result = 0;
    race_timeout_result = -1;
    race_wait_reason = PROC_WAKE_NONE;
    proc_publish_deferred_task(waker);
    proc_publish_deferred_task(race_waiter_task);

    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(5000);
    for (;;) {
        CHECK(timer_get_ticks() < limit);
        if (__atomic_load_n(&race_waiter_phase, __ATOMIC_ACQUIRE) == 1) {
            uint64_t flags = spin_lock_irqsave(&race_waiter_task->park_lock);
            int parked = race_waiter_task->state == PROC_BLOCKED &&
                         !race_waiter_task->on_cpu &&
                         race_waiter_task->park_state == PROC_PARK_PARKED;
            spin_unlock_irqrestore(&race_waiter_task->park_lock, flags);
            if (parked)
                break;
        }
        proc_yield();
    }
    proc_lifetime_snapshot(&after);
    CHECK(after.timeout_entries == 1 && after.timeout_heap_violations == 0);

    /* Run the same production expiration entry point with a due-time sample
     * while the real timer reference is live, then hold after collection so
     * CPU1 wins the park_lock through a signal wake. */
    proc_sched_expire_wait_timers(race_deadline);
    CHECK(__atomic_load_n(&race_hook_stage, __ATOMIC_ACQUIRE) == 2);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (__atomic_load_n(&race_waiter_phase, __ATOMIC_ACQUIRE) != 2) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    CHECK(race_wake_result == 1 && race_timeout_result == 0 &&
          race_wait_reason == PROC_WAKE_EVENT);
    CHECK(race_waiter_cpu == 1 && race_waker_cpu == 1 &&
          race_expirer_cpu == 0);
    int status = -1;
    CHECK(proc_wait4(race_waiter_pid, &status, 0) == race_waiter_pid &&
          status == 0);
    CHECK(proc_wait4(race_waker_pid, &status, 0) == race_waker_pid &&
          status == 0);
    proc_put(waker);
    proc_put(race_waiter_task);
    race_waiter_task = NULL;
    proc_lifetime_snapshot(&after);
    CHECK(after.timeout_entries == 0 && after.timeout_heap_violations == 0 &&
          after.task_refs == before.task_refs);
    printf("[WAIT_TIMER_CANCEL_EXPIRE_RACE: PASS waiter_cpu=%u waker_cpu=%u expirer_cpu=%u reason=event timeout_lost=1 entries=0 refs=stable]\n",
           race_waiter_cpu, race_waker_cpu, race_expirer_cpu);
}
#endif

static void __attribute__((unused)) controller(void) {
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
    CHECK(cpu_current_id() == 0);
    proc_lifetime_stats_t timer_stats;
    proc_lifetime_snapshot(&timer_stats);
#if SMP_WAIT_TIMER_CAPACITY_TEST
    CHECK(timer_stats.timeout_capacity == 1 &&
          timer_stats.timeout_entries == 1 &&
          timer_stats.timeout_heap_violations == 0);
    unsigned long refs_before_failure = timer_stats.task_refs;
    CHECK(timer_stats.wait_entries > 0);

    proc_wait_token_t full = proc_park_prepare(
        PROC_WAIT_UNINTERRUPTIBLE, timer_get_ticks() + MS_TO_TICKS(60000));
    CHECK(full.task == NULL && full.seq == 0 &&
          full.prepare_error == PROC_PARK_PREPARE_TIMEOUT_CAPACITY);
    proc_lifetime_snapshot(&timer_stats);
    CHECK(timer_stats.timeout_entries == 1 &&
          timer_stats.timeout_full_failures >= 1 &&
          timer_stats.task_refs == refs_before_failure &&
          timer_stats.timeout_heap_violations == 0);
    task_t *self = proc_current();
    uint64_t self_flags = spin_lock_irqsave(&self->park_lock);
    CHECK(self->park_state == PROC_PARK_IDLE && self->wait_deadline == 0 &&
          self->wait_mode == PROC_WAIT_UNINTERRUPTIBLE &&
          self->wake_reason == PROC_WAKE_NONE && self->wait_timer_index == -1);
    spin_unlock_irqrestore(&self->park_lock, self_flags);
    printf("[PARK_WAIT_TIMER_FULL: PASS capacity=%lu entries=%lu failures=%lu rollback=idle refs=stable]\n",
           timer_stats.timeout_capacity, timer_stats.timeout_entries,
           timer_stats.timeout_full_failures);
#else
    CHECK(timer_stats.timeout_capacity >= 1 &&
          timer_stats.timeout_entries >= 1 &&
          timer_stats.timeout_heap_violations == 0);
#endif

    CHECK(allocation[0] == 0xa20c1234);
    allocation[0] = 0x5a20abcd;
    uint64_t worker_flags = spin_lock_irqsave(&worker_task->park_lock);
    unsigned waiter_affinity = worker_task->cpu_id;
    int waiter_state = worker_task->state;
    int waiter_on_cpu = worker_task->on_cpu;
    int waiter_park_state = worker_task->park_state;
    int waiter_timer_index = worker_task->wait_timer_index;
    spin_unlock_irqrestore(&worker_task->park_lock, worker_flags);
    printf("[PARK_WAIT_CROSS_CPU_WAKE: observed cpu=%u affinity=%u state=%d on_cpu=%d park=%d timer_index=%d]\n",
           worker_park_cpu, waiter_affinity, waiter_state, waiter_on_cpu,
           waiter_park_state, waiter_timer_index);
    CHECK(waiter_state == PROC_BLOCKED && !waiter_on_cpu &&
          waiter_park_state == PROC_PARK_PARKED && waiter_timer_index >= 0 &&
          waiter_affinity == remote_cpu && worker_park_cpu == remote_cpu &&
          remote_cpu == 1);
    CHECK(cpu_current_id() == 0 && cpu_current_id() != worker_park_cpu);
    CHECK(smp_membarrier_sync_all() ==
          0); /* Actual remote IPI acknowledgments. */
    uint64_t wait_queue_flags = spin_lock_irqsave(&wait_queue_test.lock);
    int wait_queue_linked = wait_queue_test_entry.queue == &wait_queue_test &&
                            wait_queue_test_entry.linked &&
                            wait_queue_test_entry.task == worker_task &&
                            wait_queue_test_entry.key == WAIT_QUEUE_TEST_KEY &&
                            wait_queue_test.head == &wait_queue_test_entry;
    spin_unlock_irqrestore(&wait_queue_test.lock, wait_queue_flags);
    CHECK(wait_queue_linked);
    proc_lifetime_stats_t wait_queue_before, wait_queue_after;
    proc_lifetime_snapshot(&wait_queue_before);
    CHECK(wait_queue_before.wait_entries > 0);
    CHECK(wait_queue_wake_one(&wait_queue_test, WAIT_QUEUE_TEST_KEY,
                              PROC_WAKE_EVENT) == 1);
    wait_queue_flags = spin_lock_irqsave(&wait_queue_test.lock);
    int wait_queue_detached = !wait_queue_test_entry.queue &&
                              !wait_queue_test_entry.linked &&
                              !wait_queue_test_entry.task &&
                              !wait_queue_test_entry.next &&
                              !wait_queue_test_entry.prev &&
                              !wait_queue_test_entry.wait_seq &&
                              !wait_queue_test.head;
    spin_unlock_irqrestore(&wait_queue_test.lock, wait_queue_flags);
    CHECK(wait_queue_detached);
    proc_lifetime_snapshot(&wait_queue_after);
    CHECK(wait_queue_after.wait_entries + 1 == wait_queue_before.wait_entries);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (__atomic_load_n(&wait_queue_test_phase, __ATOMIC_ACQUIRE) != 2) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    printf("[WAIT_QUEUE_OWNER_WAKE: PASS waiter_cpu=%u waker_cpu=%u owner_cleared=1 entries=%lu->%lu]\n",
           worker_park_cpu, cpu_current_id(), wait_queue_before.wait_entries,
           wait_queue_after.wait_entries);
    limit = timer_get_ticks() + MS_TO_TICKS(5000);
    while (__atomic_load_n(&worker_task->wait_timer_index,
                           __ATOMIC_ACQUIRE) >= 0) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    proc_lifetime_snapshot(&timer_stats);
    CHECK(__atomic_load_n(&worker_task->wait_timer_index, __ATOMIC_ACQUIRE) ==
              -1);
#if SMP_WAIT_TIMER_CAPACITY_TEST
    CHECK(
          timer_stats.timeout_entries == 0 &&
          timer_stats.timeout_heap_violations == 0);
    printf("[PARK_WAIT_CROSS_CPU_WAKE: PASS waiter_cpu=%u affinity_cpu=%u waker_cpu=%u timer_cancelled=1]\n",
           worker_park_cpu, waiter_affinity, cpu_current_id());
    int status = -1;
    CHECK(proc_wait4(worker_pid, &status, 0) == worker_pid && status == 0);
    uint64_t finished_flags = spin_lock_irqsave(&worker_task->park_lock);
    CHECK(worker_task->park_state == PROC_PARK_IDLE &&
          worker_task->wait_deadline == 0 && worker_task->wait_timer_index == -1);
    spin_unlock_irqrestore(&worker_task->park_lock, finished_flags);
    proc_lifetime_snapshot(&timer_stats);
    CHECK(timer_stats.timeout_entries == 0 &&
          timer_stats.timeout_heap_violations == 0);
    printf("[PARK_WAIT_TIMER_REAP: PASS entries=%lu heap_violations=%lu]\n",
           timer_stats.timeout_entries, timer_stats.timeout_heap_violations);
#else
    (void)timer_stats;
#endif
    proc_put(worker_task);
#if SMP_WAIT_TIMER_CAPACITY_TEST
    run_wait_timer_cancel_expire_race();
#endif
    run_rw_mutex_writer_preference_test();
    run_mutex_owner_wait_test();
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
        "[SMP-TEST] PASS cpus=%u mask=0x%x pid=%d worker_cpu=%u "
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
#if defined(CONFIG_RV32_SYNC_TEST)
    enabled = 1;
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
#if defined(CONFIG_RV32_SYNC_TEST)
    CHECK(cpu_current_id() == 0 && CONFIG_NR_CPUS == 1 &&
          smp_online_cpu_count() == 1);
    proc_lifetime_stats_t before, after;
    proc_lifetime_snapshot(&before);
    int pid = proc_alloc_deferred(rv32_sync_test_controller);
    CHECK(pid > 0);
    task_t *task = proc_find_get(pid);
    CHECK(task && set_cpu(task, 0) == 0);
    proc_publish_deferred_task(task);
    proc_put(task);
    uint64_t limit = timer_get_ticks() + MS_TO_TICKS(15000);
    while (!__atomic_load_n(&rv32_sync_test_finished, __ATOMIC_ACQUIRE)) {
        CHECK(timer_get_ticks() < limit);
        proc_yield();
    }
    int status = -1;
    CHECK(proc_wait4(pid, &status, 0) == pid && status == 0);
    do {
        proc_yield();
        proc_lifetime_snapshot(&after);
        CHECK(timer_get_ticks() < limit);
    } while (after.task_objects != before.task_objects ||
             after.pid_entries != before.pid_entries ||
             after.listed_tasks != before.listed_tasks);
    CHECK(after.task_refs == before.task_refs &&
          after.lifetime_errors == before.lifetime_errors &&
          after.state_violations == before.state_violations);
    printf("[RV32_SYNC_TEST: PASS cpus=1 completion=blocked-wake mutex=recursive-wait rwmutex=writer-preference cleanup=stable]\n");
    return;
#else
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
          after.state_violations == before.state_violations &&
          after.task_refs == before.task_refs);
    printf("[SMP-TEST] CLEANUP PASS task-objects=%lu pids=%lu listed=%lu\n",
           after.task_objects, after.pid_entries, after.listed_tasks);
    arch_local_irq_disable();
    if (saved)
        arch_local_irq_enable();
#endif
}
#endif
