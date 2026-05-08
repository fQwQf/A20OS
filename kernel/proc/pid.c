#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/string.h"

#define PID_HASH_BITS   8
#define PID_HASH_SIZE   (1U << PID_HASH_BITS)
#define PID_BITMAP_SIZE ((32768 + 63) / 64)

static spinlock_t pid_lock = SPINLOCK_INIT;
static int next_pid = 1;
static int pid_max = 32768;
static task_t *pid_hash[PID_HASH_SIZE];
static uint64_t pid_bitmap[PID_BITMAP_SIZE];

static unsigned pid_hash_index(int pid)
{
    return ((unsigned)pid) & (PID_HASH_SIZE - 1);
}

static void pid_bitmap_set(int pid)
{
    if (pid < 1 || pid > pid_max) return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    unsigned bit = (unsigned)(pid - 1) % 64;
    pid_bitmap[idx] |= (1ULL << bit);
}

static void pid_bitmap_clear(int pid)
{
    if (pid < 1 || pid > pid_max) return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    unsigned bit = (unsigned)(pid - 1) % 64;
    pid_bitmap[idx] &= ~(1ULL << bit);
}

static int ctz64(uint64_t v)
{
    if (v == 0) return 64;
    int n = 0;
    if ((v & 0xFFFFFFFF) == 0) { n += 32; v >>= 32; }
    if ((v & 0xFFFF) == 0)     { n += 16; v >>= 16; }
    if ((v & 0xFF) == 0)       { n += 8;  v >>= 8;  }
    if ((v & 0xF) == 0)        { n += 4;  v >>= 4;  }
    if ((v & 0x3) == 0)        { n += 2;  v >>= 2;  }
    if ((v & 0x1) == 0)        { n += 1; }
    return n;
}

static int pid_bitmap_find_free(int start)
{
    int limit = pid_max > 0 ? pid_max : 1;
    int word_start = (start - 1) / 64;

    for (int w = 0; w < PID_BITMAP_SIZE; w++) {
        int wi = (word_start + w) % PID_BITMAP_SIZE;
        if (pid_bitmap[wi] == ~0ULL)
            continue;
        int bit_base = wi * 64;
        uint64_t bits = pid_bitmap[wi];
        uint64_t inv = ~bits;
        int first_bit = ctz64(inv);
        int candidate = bit_base + first_bit + 1;
        if (candidate >= 1 && candidate <= limit)
            return candidate;
    }
    return -1;
}

void proc_pid_init(void)
{
    spin_init(&pid_lock);
    memset(pid_hash, 0, sizeof(pid_hash));
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    next_pid = 1;
    pid_max = 32768;
}

int proc_pid_alloc(void)
{
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    int limit = pid_max > 0 ? pid_max : 1;

    int pid = pid_bitmap_find_free(next_pid);
    if (pid < 0) {
        if (next_pid > 1)
            pid = pid_bitmap_find_free(1);
    }

    if (pid >= 1 && pid <= limit) {
        pid_bitmap_set(pid);
        next_pid = pid + 1;
        if (next_pid > limit)
            next_pid = 1;
        spin_unlock_irqrestore(&pid_lock, flags);
        return pid;
    }

    spin_unlock_irqrestore(&pid_lock, flags);
    return -EAGAIN;
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
    pid_bitmap_clear(t->pid);
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
