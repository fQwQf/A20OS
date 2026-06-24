#include "core/lock.h"

uint64_t a20_irqsave_lock(spinlock_t *lock)
{
    return spin_lock_irqsave(lock);
}

void a20_irqsave_unlock(spinlock_t *lock, uint64_t flags)
{
    spin_unlock_irqrestore(lock, flags);
}
