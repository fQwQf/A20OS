#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"

void sysv_sem_wait_queue_init(wait_queue_t *q)
{
    wait_queue_init(q);
}

void sysv_sem_wait_queue_wake_all(wait_queue_t *q)
{
    wait_queue_wake_all(q);
}

void sysv_sem_wait_queue_finish(wait_queue_t *q, wait_queue_entry_t *entry)
{
    wait_queue_finish(q, entry);
}

long sysv_sem_copy_to_user(void *dst, const void *src, size_t n)
{
    return copy_to_user(dst, src, n);
}

long sysv_sem_copy_from_user(void *dst, const void *src, size_t n)
{
    return copy_from_user(dst, src, n);
}

task_t *sysv_sem_proc_current(void)
{
    return proc_current();
}

int sysv_sem_proc_pid(task_t *task)
{
    return task ? task->pid : 0;
}

int sysv_sem_signal_task_has_unblocked(task_t *task)
{
    return signal_task_has_unblocked(task);
}

uint64_t sysv_sem_timer_get_ticks(void)
{
    return timer_get_ticks();
}

void sysv_sem_proc_set_wake_time(task_t *task, uint64_t wake_time)
{
    if (task)
        proc_set_wake_time(task, wake_time);
}

void sysv_sem_sched(void)
{
    sched();
}

void sysv_sem_task_set_blocked(task_t *task)
{
    if (task)
        task->state = PROC_BLOCKED;
}

void *sysv_sem_kmalloc(size_t size)
{
    return kmalloc(size);
}

void sysv_sem_kfree(void *ptr)
{
    kfree(ptr);
}

void *sysv_sem_memset(void *ptr, int value, size_t size)
{
    return memset(ptr, value, size);
}
