#ifndef A20_LWIP_HOST_LOCK_H
#define A20_LWIP_HOST_LOCK_H
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/cpu.h"
extern __thread int a20_host_irq_enabled;
typedef pthread_mutex_t spinlock_t;
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline int arch_irqs_enabled(void) {
    return a20_host_irq_enabled;
}
static inline void arch_local_irq_disable(void) {
    a20_host_irq_enabled = 0;
}
static inline void arch_local_irq_enable(void) {
    a20_host_irq_enabled = 1;
}
static inline void spin_lock(spinlock_t *lock) {
    assert(!arch_irqs_enabled());
    assert(!pthread_mutex_lock(lock));
}
static inline void spin_unlock(spinlock_t *lock) {
    assert(!arch_irqs_enabled());
    assert(!pthread_mutex_unlock(lock));
}
#define panic(...)                                                             \
    do {                                                                       \
        fprintf(stderr, __VA_ARGS__);                                          \
        abort();                                                               \
    } while (0)
#endif
