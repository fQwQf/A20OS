#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/slab.h"

void a20_event_wait_queue_init(wait_queue_t *q)
{
    wait_queue_init(q);
}

void a20_event_wait_queue_wake_all(wait_queue_t *q)
{
    wait_queue_wake_all(q);
}

void a20_event_wait_queue_wake_one(wait_queue_t *q)
{
    wait_queue_wake_one(q);
}

void *a20_event_kmalloc(size_t size)
{
    return kmalloc(size);
}

void a20_event_kfree(void *ptr)
{
    kfree(ptr);
}

void *a20_event_memset(void *ptr, int value, size_t size)
{
    return memset(ptr, value, size);
}

void a20_event_spin_init(spinlock_t *lock)
{
    spin_init(lock);
}

void a20_event_spin_set_debug(spinlock_t *lock, const char *name, void *container)
{
    spin_set_debug(lock, name, container);
}
