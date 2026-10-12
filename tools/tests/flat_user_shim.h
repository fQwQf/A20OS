#ifndef A20_FLAT_TEST_SHIM
#define A20_FLAT_TEST_SHIM
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>
/* Hosted dependency adapter only: production allocation/range/pin code is
 * compiled unchanged. Real kernel locks/allocator run in MCU integration. */
#define _CORE_LOCK_H
#include "core/types.h"
typedef struct spinlock {
    int held;
} spinlock_t;
#define SPINLOCK_INIT {0}
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE))
        sched_yield();
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
}
#endif
