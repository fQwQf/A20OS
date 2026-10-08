/* Compile and execute the real spin_lock_at() and preempt_disable() bodies
 * with deterministic IRQ and contention seams around CPU-slot selection. */
#include <assert.h>
#include <stdint.h>
#include "core/preempt.h"

static int test_irq_enabled = 1;
static unsigned test_cpu_id;
static int inject_migration_on_cpu_read;
static int pending_irq;
static unsigned exchange_calls;
static unsigned relax_calls;
static unsigned expected_acquire_preempt;
static unsigned expected_wait_preempt;

static int test_exchange(volatile int *ptr, int value, int order)
{
    int old = __atomic_exchange_n(ptr, value, order);
    exchange_calls++;
    if (!old)
        /* Simulate IRQ return immediately after lock ownership is acquired. */
        assert(preempt_count() == expected_acquire_preempt);
    return old;
}

/* Override only while parsing the kernel lock header. The helper is compiled
 * above the override and therefore invokes the compiler's real atomic builtin. */
#define __atomic_exchange_n(ptr, value, order) \
    test_exchange((ptr), (value), (order))
#include "core/lock.h"
#undef __atomic_exchange_n

static spinlock_t test_lock;

struct task_t { int pid; };
static struct task_t current_task;

struct task_t *proc_current(void) { return &current_task; }
int proc_task_pid(const void *task) { return ((const struct task_t *)task)->pid; }
unsigned cpu_current_id(void)
{
    unsigned observed = test_cpu_id;
    if (inject_migration_on_cpu_read) {
        inject_migration_on_cpu_read = 0;
        if (test_irq_enabled)
            test_cpu_id = 1; /* deliver an IRQ and migrate before slot update */
        else
            pending_irq = 1; /* IRQ is deferred until the slot is published */
    }
    return observed;
}
int arch_irqs_enabled(void) { return test_irq_enabled; }
void arch_local_irq_disable(void) { test_irq_enabled = 0; }
void arch_local_irq_enable(void)
{
    if (pending_irq) {
        pending_irq = 0;
        /* Model the IRQ-return preemption decision before restoring IRQs. */
        if (preempt_count() == 0)
            test_cpu_id = 1;
    }
    test_irq_enabled = 1;
}
void arch_cpu_relax(void)
{
    relax_calls++;
    assert(preempt_count() == expected_wait_preempt);
    __atomic_store_n(&test_lock.locked, 0, __ATOMIC_RELEASE);
}
uint64_t timer_get_ticks(void) { return 0; }
int arch_unwind_frames(uint64_t frame, struct backtrace_frame *out,
                       unsigned capacity)
{
    (void)frame;
    (void)out;
    (void)capacity;
    return 0;
}
void kallsyms_print(uint64_t addr) { (void)addr; }

/* Exercise the actual per-CPU implementation rather than a test double. */
#include "../../kernel/core/preempt.c"

static void check_acquire(unsigned cpu, unsigned initial_preempt, int irq_mode)
{
    test_cpu_id = cpu;
    inject_migration_on_cpu_read = 0;
    spin_init(&test_lock);
    preempt_state_init(0);
    preempt_state_init(1);
    for (unsigned i = 0; i < initial_preempt; i++)
        preempt_disable();
    inject_migration_on_cpu_read = 1;
    expected_acquire_preempt = initial_preempt + 1;
    expected_wait_preempt = initial_preempt;
    unsigned calls_before = exchange_calls;

    if (irq_mode == 1) {
        test_irq_enabled = 1;
        uint64_t flags = spin_lock_irqsave(&test_lock);
        assert(!test_irq_enabled);
        assert(preempt_count() == expected_acquire_preempt);
        spin_unlock_irqrestore(&test_lock, flags);
        assert(test_irq_enabled);
    } else if (irq_mode == 2) {
        test_irq_enabled = 0;
        spin_lock(&test_lock);
        assert(!test_irq_enabled);
        assert(test_cpu_id == cpu);
        assert(preempt_count() == expected_acquire_preempt);
        spin_unlock(&test_lock);
        assert(!test_irq_enabled);
        arch_local_irq_enable();
        assert(test_irq_enabled);
    } else {
        spin_lock(&test_lock);
        assert(test_cpu_id == cpu); /* pending IRQ could not migrate the owner */
        assert(preempt_count() == expected_acquire_preempt);
        spin_unlock(&test_lock);
    }
    assert(exchange_calls == calls_before + 1);
    assert(preempt_count() == initial_preempt);
    while (preempt_count())
        preempt_enable();
}

static void check_contended(unsigned initial_preempt)
{
    test_cpu_id = 0;
    test_irq_enabled = 1;
    inject_migration_on_cpu_read = 0;
    pending_irq = 0;
    preempt_state_init(0);
    preempt_state_init(1);
    for (unsigned i = 0; i < initial_preempt; i++)
        preempt_disable();
    spin_init(&test_lock);
    test_lock.locked = 1;
    expected_acquire_preempt = initial_preempt + 1;
    expected_wait_preempt = initial_preempt;
    unsigned calls_before = exchange_calls;
    unsigned relax_before = relax_calls;

    spin_lock(&test_lock);
    assert(exchange_calls == calls_before + 2);
    assert(relax_calls == relax_before + 1);
    assert(test_lock.contended_acquires == 1);
    assert(preempt_count() == initial_preempt + 1);
    spin_unlock(&test_lock);
    assert(preempt_count() == initial_preempt);
    assert(test_lock.locked == 0);
    while (preempt_count())
        preempt_enable();
}

int main(void)
{
    check_acquire(0, 0, 0);
    check_acquire(0, 1, 0);
    check_acquire(0, 0, 1);
    check_acquire(0, 0, 2);

    /* Failed attempts preserve zero and nonzero caller nesting while waiting. */
    check_contended(0);
    check_contended(1);
    return 0;
}
