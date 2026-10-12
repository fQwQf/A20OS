#ifdef CONFIG_MCU_SHARED_TEST
#include "shared_kernel.h"
#include "core/arch.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "core/lock.h"
#include "mm/slab.h"
#include "proc/proc.h"

static task_t *worker_task;
static unsigned phase;
static unsigned count32;
static uint64_t count64;
static void *shared_allocation;
static uintptr_t worker_stack;
static int worker_pid;
static unsigned done;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition))                                                      \
            panic("shared selftest: " #condition);                             \
    } while (0)

static void atomic_probe(void) {
    /* Verify the libcall ABI as emitted by this compiler, including failed
     * CAS updating expected and a carry across the two 32-bit bus words. */
    uint8_t b = 7, be = 7;
    uint16_t h = 1000, he = 0;
    uint32_t w = 55, we = 55;
    uint64_t d = 0xffffffffULL, de = 0;
    CHECK(__atomic_compare_exchange_n(&b, &be, 9, 0, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST));
    CHECK(!__atomic_compare_exchange_n(&h, &he, 3, 0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST));
    CHECK(he == 1000 && h == 1000 && b == 9);
    CHECK(__atomic_compare_exchange_n(&w, &we, 56, 1, __ATOMIC_SEQ_CST,
                                      __ATOMIC_SEQ_CST));
    CHECK(!__atomic_compare_exchange_n(&d, &de, 1, 0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST));
    CHECK(de == 0xffffffffULL);
    CHECK(__atomic_add_fetch(&d, 1, __ATOMIC_SEQ_CST) == 0x100000000ULL);
    CHECK(__atomic_exchange_n(&w, 77, __ATOMIC_SEQ_CST) == 56);
    CHECK(__atomic_fetch_xor(&h, 7, __ATOMIC_SEQ_CST) == 1000);
    CHECK(__atomic_fetch_or(&b, 2, __ATOMIC_SEQ_CST) == 9);
    CHECK(__atomic_fetch_and(&b, 3, __ATOMIC_SEQ_CST) == 11);
    CHECK(__atomic_sub_fetch(&d, 1, __ATOMIC_SEQ_CST) == 0xffffffffULL);
}

static void count_operations(void) {
    for (unsigned i = 0; i < 128; i++) {
        __atomic_fetch_add(&count32, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&count64, 1, __ATOMIC_RELAXED);
        void *p = kmalloc(37 + (i & 7U));
        CHECK(p != NULL);
        CHECK((uintptr_t)p >= 0x60000000UL);
        *(uint32_t *)p = i;
        CHECK(*(uint32_t *)p == i);
        kfree(p);
    }
}

static void worker(void) {
    CHECK(arch_current_cpu_id() == 1);
    worker_pid = proc_current()->pid;
    volatile uint32_t local[4] = {0xa201, 0xa202, 0xa203, 0xa204};
    worker_stack = (uintptr_t)&local[0];
    CHECK(worker_stack >= 0x60000000UL);
    atomic_probe();
    count_operations();
    uint32_t *p = kmalloc(64);
    CHECK(p != NULL);
    p[0] = 0xa20c1234;
    p[15] = 0x1234c20a;
    shared_allocation = p;
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
    CHECK(token.seq != 0);
    __atomic_store_n(&phase, 1, __ATOMIC_RELEASE);
    CHECK(proc_park_commit(token) == PROC_WAKE_EVENT);
    proc_park_finish(token);
    CHECK(__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == 2);
    CHECK(arch_current_cpu_id() == 0);
    CHECK(proc_current()->pid == worker_pid);
    uint64_t deadline = timer_get_ticks() + 3;
    proc_sleep_until(deadline);
    CHECK(timer_get_ticks() >= deadline);
    CHECK(worker_stack == (uintptr_t)&local[0]);
    CHECK(local[0] == 0xa201 && local[3] == 0xa204);
    CHECK(shared_allocation == p && p[0] == 0xa20c1234);
    CHECK(p[15] == 0x1234c20a);
    kfree(p); /* Allocate on MCU1, free on MCU0. */
    CHECK(__atomic_load_n(&count32, __ATOMIC_ACQUIRE) == 256);
    CHECK(__atomic_load_n(&count64, __ATOMIC_ACQUIRE) == 0x100000080ULL);
    printf("[SHARED-TEST] PASS pid=%d migrated=1->0 stack=0x%x "
           "atomics=1/2/4/8 count=256 heap=cross-cpu\n",
           worker_pid, (unsigned)worker_stack);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    proc_exit(0);
}

static void controller(void) {
    CHECK(arch_current_cpu_id() == 0);
    atomic_probe();
    count_operations();
    for (;;) {
        if (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) == 1) {
            uint64_t f = spin_lock_irqsave(&worker_task->park_lock);
            int blocked =
                worker_task->state == PROC_BLOCKED && !worker_task->on_cpu;
            spin_unlock_irqrestore(&worker_task->park_lock, f);
            if (blocked)
                break;
        }
        proc_yield();
    }
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = 1,
    };
    CHECK(proc_sched_set(worker_task, &config) == 0);
    __atomic_store_n(&phase, 2, __ATOMIC_RELEASE);
    CHECK(proc_try_wake(worker_task, worker_task->wait_seq, PROC_WAKE_EVENT));
    /* Retain the worker reference through exit, then let the normal lifecycle
     * retire it. The test does not inject scheduler state from the host. */
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE))
        proc_yield();
    proc_put(worker_task);
    proc_exit(0);
}

void mcu_shared_selftest_start(void) {
    CHECK(CONFIG_NR_CPUS >= 2);
    __atomic_store_n(&count64, 0xffffff80ULL, __ATOMIC_RELAXED);
    /* The boot idle parent's affinity is CPU0, so neither newborn can run
     * elsewhere before its explicit pin is published. */
    int pid = proc_alloc(worker);
    CHECK(pid > 0);
    worker_task = proc_find_get(pid);
    CHECK(worker_task != NULL);
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = 2,
    };
    CHECK(proc_sched_set(worker_task, &config) == 0);
    CHECK(proc_alloc(controller) > 0);
}
#endif
