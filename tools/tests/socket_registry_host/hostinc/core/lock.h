#ifndef _CORE_LOCK_H
#define _CORE_LOCK_H
/*
 * Host stand-in for core/lock.h, used only by tools/tests/socket_registry_host.
 *
 * The production lock body reaches the architecture's IRQ predicates, which are
 * `cli`/`sti` on x86_64 and cannot execute in a host process. What the registry
 * state machine needs from a lock is mutual exclusion and nothing else, so the
 * host lock keeps the production struct layout and the production API but drops
 * the IRQ and preemption bookkeeping -- the differential harness is
 * single-threaded, so the flag alone is sufficient.
 *
 * Both selector runs use this same shim, so it cannot bias the comparison
 * between the C reference bodies and the Rust state machine.
 */
#include <stdint.h>
#include <stddef.h>

struct lock_callsite_sample;

typedef struct spinlock {
    volatile int locked;
    void *owner;
    uintptr_t owner_ra;
    const char *name;
    void *container;
    uint64_t contended_acquires;
    uint64_t contended_spins;
    uint64_t contended_max_spins;
    struct lock_callsite_sample *samples;
    struct spinlock *counter_sink;
} spinlock_t;

#define LOCK_CALLSITE_SAMPLES 32
typedef struct lock_callsite_sample {
    uintptr_t ra;
    uint64_t contended;
    uint64_t spins;
    uint64_t max_spins;
} lock_callsite_sample_t;

#define SPINLOCK_INIT { 0, NULL, 0, NULL, NULL, 0, 0, 0, NULL, NULL }

static inline void spin_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->owner = NULL;
    lock->owner_ra = 0;
    lock->name = NULL;
    lock->container = NULL;
    lock->contended_acquires = 0;
    lock->contended_spins = 0;
    lock->contended_max_spins = 0;
    lock->samples = NULL;
    lock->counter_sink = NULL;
}

static inline void spin_set_debug(spinlock_t *lock, const char *name,
                                 void *container)
{
    lock->name = name;
    lock->container = container;
}

static inline void spin_lock(spinlock_t *lock)
{
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE))
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline void spin_unlock(spinlock_t *lock)
{
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    spin_lock(lock);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    (void)flags;
    spin_unlock(lock);
}

static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE))
        return 0;
    *flags = 0;
    return 1;
}

#endif /* _CORE_LOCK_H */
