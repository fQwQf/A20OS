/*
 * fscompat/core/lock.h — a no-op spinlock for the user-space FS host.
 *
 * The service process runs single-threaded, so lock operations degrade to
 * no-ops; only the type and API shape are kept, so that kernel disk filesystem
 * sources can compile as-is into user space
 * (docs/hybrid-kernel/06-user-fs.md).
 */
#ifndef _LOCK_H
#define _LOCK_H

#include "core/types.h"

typedef struct spinlock {
    int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t *l)
{
    l->locked = 0;
}

static inline void spin_lock(spinlock_t *l)
{
    l->locked = 1;
}

static inline void spin_unlock(spinlock_t *l)
{
    l->locked = 0;
}

static inline unsigned long spin_lock_irqsave(spinlock_t *l)
{
    l->locked = 1;
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags)
{
    (void)flags;
    l->locked = 0;
}

#endif /* _LOCK_H */
