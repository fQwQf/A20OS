#ifndef _CORE_LOCK_H
#define _CORE_LOCK_H

#include "core/types.h"
#include "core/defs.h"
#include "core/klog.h"

#if CONFIG_DEBUG_LOCKS
#include "proc/proc.h"
#endif

typedef struct spinlock {
    volatile int locked;
#if CONFIG_DEBUG_LOCKS
    void *owner;
    uintptr_t owner_ra;
    const char *name;
    void *container;
#endif
} spinlock_t;

#if CONFIG_DEBUG_LOCKS
#define SPINLOCK_INIT { 0, NULL, 0, NULL, NULL }
#else
#define SPINLOCK_INIT { 0 }
#endif

static inline void spin_init(spinlock_t *lock) {
    lock->locked = 0;
#if CONFIG_DEBUG_LOCKS
    lock->owner = NULL;
    lock->owner_ra = 0;
    lock->name = NULL;
    lock->container = NULL;
#endif
}

static inline void spin_set_debug(spinlock_t *lock, const char *name, void *container) {
#if CONFIG_DEBUG_LOCKS
    if (!lock)
        return;
    lock->name = name;
    lock->container = container;
#else
    (void)lock;
    (void)name;
    (void)container;
#endif
}

static inline void spin_lock_at(spinlock_t *lock, uintptr_t caller_ra) {
#if CONFIG_DEBUG_LOCKS
    uint64_t spins = 0;
    task_t *cur = proc_current();
    uintptr_t waiter_ra = caller_ra ? caller_ra : (uintptr_t)__builtin_return_address(0);
#else
    (void)caller_ra;
#endif
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
#if CONFIG_DEBUG_LOCKS
            if ((++spins & ((1UL << 24) - 1)) == 0) {
                task_t *owner = (task_t *)lock->owner;
                printf("[LOCK] spin wait: lock=%p name=%s container=%p waiter=%p/%d owner=%p/%d owner_ra=0x%lx waiter_ra=0x%lx spins=%lu\n",
                       (void *)lock,
                       lock->name ? lock->name : "?",
                       lock->container,
                       (void *)cur, cur ? cur->pid : -1,
                       (void *)owner, owner ? owner->pid : -1,
                       (unsigned long)lock->owner_ra,
                       (unsigned long)waiter_ra, spins);
            }
#endif
            __asm__ volatile("" ::: "memory");
        }
    }
    mb();
#if CONFIG_DEBUG_LOCKS
    lock->owner = cur;
    lock->owner_ra = waiter_ra;
#endif
}

static inline void spin_lock(spinlock_t *lock) {
    spin_lock_at(lock, (uintptr_t)__builtin_return_address(0));
}

static inline void spin_unlock(spinlock_t *lock) {
    mb();
#if CONFIG_DEBUG_LOCKS
    lock->owner = NULL;
    lock->owner_ra = 0;
#endif
    __sync_lock_release(&lock->locked);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    spin_lock_at(lock, (uintptr_t)__builtin_return_address(0));
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags)
        arch_local_irq_enable();
}

#endif /* _CORE_LOCK_H */
