#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/task_object.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/string.h"

#ifdef CONFIG_MCU
/* STM32 has very limited SRAM; keep the PID namespace small. */
#define PID_MAX_LIMIT 256
#define PID_HASH_BITS 4
#else
#define PID_MAX_LIMIT 4194304
#define PID_HASH_BITS 10
#endif

#define PID_HASH_SIZE (1U << PID_HASH_BITS)

/* PID_IMPL=rust moves the id space itself — occupancy bitmap, next-id cursor,
 * pid_max limit and the free-id search including the wrap — to
 * kernel/proc/pid_alloc.rs.  C keeps the pid_lock that serializes it, the
 * task_t hash chain used for lookup, and the lifetime accounting. */
#ifdef CONFIG_PID_RUST
_Static_assert(PID_MAX_LIMIT == 4194304,
               "CONFIG_PID_RUST needs the non-MCU PID_MAX_LIMIT that "
               "kernel/proc/pid_alloc.rs is sized for");
extern void a20_pid_alloc_init(void);
extern int a20_pid_alloc(void);
extern void a20_pid_release(int pid);
extern int a20_pid_max(void);
extern int a20_pid_next_value(void);
extern int a20_pid_set_max(int value);
#else
#define PID_BITMAP_SIZE ((PID_MAX_LIMIT + 63) / 64)

static int next_pid = 1;
static int pid_max = PID_MAX_LIMIT < 32768 ? PID_MAX_LIMIT : 32768;
static uint64_t pid_bitmap[PID_BITMAP_SIZE];

static void pid_bitmap_set(int pid) {
    if (pid < 1 || pid > pid_max)
        return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    unsigned bit = (unsigned)(pid - 1) % 64;
    pid_bitmap[idx] |= (1ULL << bit);
}

static void pid_bitmap_clear(int pid) {
    if (pid < 1 || pid > PID_MAX_LIMIT)
        return;
    unsigned idx = (unsigned)(pid - 1) / 64;
    unsigned bit = (unsigned)(pid - 1) % 64;
    pid_bitmap[idx] &= ~(1ULL << bit);
}

static int ctz64(uint64_t v) {
    if (v == 0)
        return 64;
    int n = 0;
    if ((v & 0xFFFFFFFF) == 0) {
        n += 32;
        v >>= 32;
    }
    if ((v & 0xFFFF) == 0) {
        n += 16;
        v >>= 16;
    }
    if ((v & 0xFF) == 0) {
        n += 8;
        v >>= 8;
    }
    if ((v & 0xF) == 0) {
        n += 4;
        v >>= 4;
    }
    if ((v & 0x3) == 0) {
        n += 2;
        v >>= 2;
    }
    if ((v & 0x1) == 0) {
        n += 1;
    }
    return n;
}

static int pid_bitmap_find_free(int start) {
    int limit = pid_max > 0 ? pid_max : 1;
    int word_start = (start - 1) / 64;

    for (int w = 0; w < PID_BITMAP_SIZE; w++) {
        int wi = (word_start + w) % PID_BITMAP_SIZE;
        if (pid_bitmap[wi] == ~0ULL)
            continue;
        int bit_base = wi * 64;
        uint64_t bits = pid_bitmap[wi];
        uint64_t inv = ~bits;
        if (wi == word_start) {
            int start_bit = (start - 1) % 64;
            if (start_bit > 0)
                inv &= ~((1ULL << start_bit) - 1);
        }
        int first_bit = ctz64(inv);
        if (first_bit == 64)
            continue;
        int candidate = bit_base + first_bit + 1;
        if (candidate >= 1 && candidate <= limit)
            return candidate;
    }
    return -1;
}

/* Caller holds pid_lock. */
static int pid_alloc_locked(void) {
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
        return pid;
    }

    return -EAGAIN;
}
#endif /* CONFIG_PID_RUST */

static spinlock_t pid_lock = SPINLOCK_INIT;
static task_t *pid_hash[PID_HASH_SIZE];
/* PID 1 belongs to the first userspace init, never an early device worker. */
static int boot_init_claimed;

static unsigned pid_hash_index(int pid) {
    return (((unsigned)pid) * 2654435761U) >> (32 - PID_HASH_BITS);
}

/* Return one id to the id space.  The caller holds pid_lock. */
static void proc_pid_release_id(int pid) {
    if (pid == 1)
        return; /* permanent reservation, including failed init construction */
#ifdef CONFIG_PID_RUST
    a20_pid_release(pid);
#else
    pid_bitmap_clear(pid);
#endif
}

void proc_pid_init(void) {
    spin_init(&pid_lock);
    memset(pid_hash, 0, sizeof(pid_hash));
#ifdef CONFIG_PID_RUST
    a20_pid_alloc_init();
#else
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    next_pid = 1;
    pid_max = PID_MAX_LIMIT < 32768 ? PID_MAX_LIMIT : 32768;
#endif
#ifdef CONFIG_PID_RUST
    int reserved = a20_pid_alloc();
#else
    int reserved = pid_alloc_locked();
#endif
    if (reserved != 1)
        panic("pid allocator could not reserve boot init");
    boot_init_claimed = 0;
}

int proc_pid_claim_init(void)
{
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    int pid = boot_init_claimed ? -EEXIST : 1;
    boot_init_claimed = 1;
    spin_unlock_irqrestore(&pid_lock, flags);
    return pid;
}

int proc_pid_alloc(void) {
    uint64_t flags = spin_lock_irqsave(&pid_lock);
#ifdef CONFIG_PID_RUST
    int pid = a20_pid_alloc();
#else
    int pid = pid_alloc_locked();
#endif
    spin_unlock_irqrestore(&pid_lock, flags);
    return pid;
}

void proc_pid_register(task_t *t) {
    if (!t || !proc_get(t))
        return;

    uint64_t flags = spin_lock_irqsave(&pid_lock);
    unsigned h = pid_hash_index(t->pid);
    t->pid_hash_next = pid_hash[h];
    pid_hash[h] = t;
    proc_lifetime_note_pid_add();
    spin_unlock_irqrestore(&pid_lock, flags);
}

void proc_pid_unregister(task_t *t) {
    if (!t)
        return;

    int removed = 0;
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    unsigned h = pid_hash_index(t->pid);
    task_t **pp = &pid_hash[h];
    while (*pp) {
        if (*pp == t) {
            *pp = t->pid_hash_next;
            t->pid_hash_next = NULL;
            removed = 1;
            break;
        }
        pp = &(*pp)->pid_hash_next;
    }
#ifndef A20_TASK_OBJECTS_ENABLED
    if (removed)
        proc_pid_release_id(t->pid);
#endif
    spin_unlock_irqrestore(&pid_lock, flags);
    if (removed) {
        proc_lifetime_note_pid_remove();
        proc_put(t);
    }
}

#ifdef A20_TASK_OBJECTS_ENABLED
/* Pinned Native operations still call PID-based core APIs. Keep the number
 * reserved until the last task reference so those calls cannot hit a successor.
 * Handles retain only the identity object, allowing normal PID recycling. */
void proc_pid_final_release(task_t *task) {
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    proc_pid_release_id(task->pid);
    spin_unlock_irqrestore(&pid_lock, flags);
}
#endif

task_t *proc_find_get(int pid) {
    uint64_t flags = spin_lock_irqsave(&pid_lock);
    task_t *t = pid_hash[pid_hash_index(pid)];
    while (t) {
        if (t->pid == pid && t->state != PROC_UNUSED)
            break;
        t = t->pid_hash_next;
    }
    if (t && !proc_get(t))
        t = NULL;
    spin_unlock_irqrestore(&pid_lock, flags);
    return t;
}

int proc_pid_max(void) {
#ifdef CONFIG_PID_RUST
    return a20_pid_max();
#else
    return pid_max;
#endif
}

int proc_pid_next_value(void) {
#ifdef CONFIG_PID_RUST
    return a20_pid_next_value();
#else
    return next_pid;
#endif
}

int proc_set_pid_max(int value) {
    if (value < 1 || value > PID_MAX_LIMIT)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&pid_lock);
#ifdef CONFIG_PID_RUST
    int rc = a20_pid_set_max(value);
#else
    pid_max = value;
    if (next_pid < 1 || next_pid > pid_max)
        next_pid = 1;
    int rc = 0;
#endif
    spin_unlock_irqrestore(&pid_lock, flags);
    return rc;
}
