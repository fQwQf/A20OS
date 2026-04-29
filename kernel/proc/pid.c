#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/string.h"

#define PID_HASH_BITS 8
#define PID_HASH_SIZE (1U << PID_HASH_BITS)

static spinlock_t pid_lock = SPINLOCK_INIT;
static int next_pid = 1;
static int pid_max = 32768;
static task_t *pid_hash[PID_HASH_SIZE];

static unsigned pid_hash_index(int pid)
{
    return ((unsigned)pid) & (PID_HASH_SIZE - 1);
}

static int pid_in_use_locked(int pid)
{
    task_t *t = pid_hash[pid_hash_index(pid)];
    while (t) {
        if (t->pid == pid && t->state != PROC_UNUSED)
            return 1;
        t = t->pid_hash_next;
    }
    return 0;
}

void proc_pid_init(void)
{
    spin_init(&pid_lock);
    memset(pid_hash, 0, sizeof(pid_hash));
    next_pid = 1;
    pid_max = 32768;
}

int proc_pid_alloc(void)
{
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    int limit = pid_max > 0 ? pid_max : 1;
    int pid = -EAGAIN;

    for (int i = 0; i < limit; i++) {
        if (next_pid < 1 || next_pid > limit)
            next_pid = 1;
        int candidate = next_pid++;
        if (!pid_in_use_locked(candidate)) {
            pid = candidate;
            break;
        }
    }

    spin_unlock_irqrestore(&pid_lock, flags);
    return pid;
}

void proc_pid_register(task_t *t)
{
    if (!t)
        return;

    uint64_t flags = spin_lock_irqsave(&pid_lock);
    unsigned h = pid_hash_index(t->pid);
    t->pid_hash_next = pid_hash[h];
    pid_hash[h] = t;
    spin_unlock_irqrestore(&pid_lock, flags);
}

void proc_pid_unregister(task_t *t)
{
    if (!t)
        return;

    uint64_t flags = spin_lock_irqsave(&pid_lock);
    unsigned h = pid_hash_index(t->pid);
    task_t **pp = &pid_hash[h];
    while (*pp) {
        if (*pp == t) {
            *pp = t->pid_hash_next;
            t->pid_hash_next = NULL;
            break;
        }
        pp = &(*pp)->pid_hash_next;
    }
    spin_unlock_irqrestore(&pid_lock, flags);
}

task_t *proc_find(int pid)
{
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    task_t *t = pid_hash[pid_hash_index(pid)];
    while (t) {
        if (t->pid == pid && t->state != PROC_UNUSED)
            break;
        t = t->pid_hash_next;
    }
    spin_unlock_irqrestore(&pid_lock, flags);
    return t;
}

int proc_pid_max(void)
{
    return pid_max;
}

int proc_pid_next_value(void)
{
    return next_pid;
}

int proc_set_pid_max(int value)
{
    if (value < 1 || value > 4194304)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&pid_lock);
    pid_max = value;
    if (next_pid < 1 || next_pid > pid_max)
        next_pid = 1;
    spin_unlock_irqrestore(&pid_lock, flags);
    return 0;
}
