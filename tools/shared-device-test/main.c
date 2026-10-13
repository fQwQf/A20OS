#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include "core/shared_device_call.h"
#include "core/cpu.h"

_Thread_local unsigned test_cpu;
_Thread_local int test_irq = 1;
static unsigned done, calls[CONFIG_NR_CPUS], nested_rejected;
static unsigned online_mask = (1U << CONFIG_NR_CPUS) - 1;
static pthread_barrier_t start;
void shared_test_pause(void) { sched_yield(); }
int smp_cpu_is_online(unsigned cpu) { return cpu < CONFIG_NR_CPUS && (online_mask & (1U << cpu)); }
static int operation(uint32_t owner, uint64_t argument, uint64_t *result) {
    assert(!test_irq && owner == test_cpu);
    if ((argument & 255) == 0) {
        uint64_t unused;
        assert(shared_device_call(test_cpu, operation, owner, 1, &unused) == -EDEADLK);
        __atomic_fetch_add(&nested_rejected, 1, __ATOMIC_RELAXED);
    }
    calls[test_cpu]++;
    *result = (argument * 0x100000001b3ULL) ^ owner;
    return argument & 1 ? -EAGAIN : 0;
}
static void *run(void *argument) {
    test_cpu = (unsigned)(uintptr_t)argument;
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < 20000; i++) {
        /* Reuse one caller slot across all owners, including a hot owner 0.
         * Owner changes expose the state/owner ABA window to other pollers. */
        unsigned owner = (i & 1) ? 0 : (i / 2 + test_cpu) % CONFIG_NR_CPUS;
        uint64_t input = ((uint64_t)test_cpu << 32) | i, result = 0;
        test_irq = i & 1;
        int status = shared_device_call(owner, operation, owner, input, &result);
        assert(status == ((input & 1) ? -EAGAIN : 0));
        assert(result == ((input * 0x100000001b3ULL) ^ owner));
        assert(test_irq == (int)(i & 1));
    }
    __atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != CONFIG_NR_CPUS)
        cpu_relax();
    return NULL;
}
int main(void) {
    uint64_t value = 37;
    assert(shared_device_call(CONFIG_NR_CPUS, operation, 0, 0, &value) == -EINVAL);
    assert(shared_device_call(0, NULL, 0, 0, &value) == -EINVAL);
    assert(shared_device_call(0, operation, 0, 0, NULL) == -EINVAL);
    online_mask = 1;
    assert(shared_device_call(1, operation, 1, 0, &value) == -ENODEV);
    online_mask = (1U << CONFIG_NR_CPUS) - 1;
    assert(test_irq == 1 && value == 37);
    pthread_t threads[CONFIG_NR_CPUS];
    assert(!pthread_barrier_init(&start, NULL, CONFIG_NR_CPUS));
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++)
        assert(!pthread_create(&threads[i], NULL, run, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++) assert(!pthread_join(threads[i], NULL));
    unsigned total = 0;
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++) { assert(calls[i]); total += calls[i]; }
    assert(total == 20000 * CONFIG_NR_CPUS && nested_rejected == 79 * CONFIG_NR_CPUS);
    assert(!pthread_barrier_destroy(&start));
    printf("PASS shared device calls=%u endpoints=%u nested=%u owner-reuse=checked irq-restore=checked\n",
           total, CONFIG_NR_CPUS, nested_rejected);
}
