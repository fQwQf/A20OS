#include <core/lock.h>
#include <proc/proc.h>

void a20_spin_lock(spinlock_t *lock)
{
    spin_lock(lock);
}

void a20_spin_unlock(spinlock_t *lock)
{
    spin_unlock(lock);
}

int a20_proc_has_cap(task_t *t, int cap)
{
    return proc_has_cap(t, cap);
}
