#ifndef _CORE_LOCK_H
#define _CORE_LOCK_H

#include "core/types.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/cpu.h"
#include "core/timer.h"

#if CONFIG_DEBUG_LOCKS
#include "proc/proc.h"
#endif

struct task_t;
extern struct task_t *proc_current(void);
extern int proc_task_pid(const void *task);

/*
 * Global lock order contract (outermost -> innermost).
 * For the full driver-private lock contracts see
 * docs/drivers/guide/lock-order.md.
 *
 * Scheduler/task locks (lock-serialization-split §1.2).  There is no global
 * proc_lock any more; the states it used to cover are split three ways:
 *   tasklist_lock  task-list membership (all_next/all_prev) and the
 *                  parent/children and thread-group chains.  NOT scheduling
 *                  state.
 *   park_lock      one task's ->state / ->park_state / ->wait_seq /
 *                  ->wake_reason / ->on_cpu, plus the per-task fields that
 *                  travel with it (->mm, ->files, ->cred, ptrace state).
 *   runq_lock      one CPU's queue membership (->on_rq / ->cpu_id /
 *                  ->sched_level / ->ready_since) and the ->dispatching /
 *                  ->owner_cpu publication.
 *   g_cpu_switch_out[cpu]  the per-CPU outgoing-task slot; strictly outside
 *                  park_lock and not part of the task address order.
 *
 * Global order:
 *   cg_node.lock -> tasklist_lock -> park_lock -> runq_lock -> pfa.lock
 *   tasklist_lock -> park_lock
 *   tasklist_lock -> runq_lock
 *   park_lock -> signal_state.lock
 *   park_lock -> g_wait_timer_lock
 *   park_lock -> files_struct.lock -> VFS global-file/vnode locks
 *   park_lock -> mm_struct.lock
 *   park_lock -> a20_handle_table.lock
 *   driver registry/IRQ locks -> device-private locks
 *   g_lwip_lock -> net_bucket[*]
 *   g_lwip_lock -> virtio-net nonblocking send/recv paths only
 *
 * cg_node.lock is never held together with tasklist_lock: the memory
 * controller releases it before scanning tasks for an OOM victim.
 *
 * Rules:
 *
 * Lock-safe network entry points (see docs/net/network-lock-contract.md):
 * - a20_lwip_lock()/a20_lwip_unlock(): outer lock around all lwIP core calls.
 * - a20_lwip_poll_locked(): progress entry that runs with g_lwip_lock held;
 *   must not allocate, block, or acquire a socket-table bucket lock.
 * - a20_lwip_poll(): acquires g_lwip_lock, runs progress, releases it, then
 *   runs the socket bottom-half under the socket-table bucket lock only.
 * - lwIP callbacks run under g_lwip_lock and must only stage events into the
 *   preallocated per-PCB ring; allocation, enqueue, and wakeup happen in the
 *   bottom-half with a socket-table bucket lock held.
 *
 * Socket-table shard locks (lock-serialization-split §2; text handed over by
 * the net implementer, merged during integration):
 * - The socket registry is sharded into net_bucket[0 .. NET_SOCK_BUCKETS-1],
 *   one lock per contiguous run of NET_SOCK_SLOTS_PER_BUCKET registry slots
 *   (512 on the server profile, 32 on the default one, 1 on the embedded one;
 *   see socket_internal.h).  The bucket of a socket is reg_idx >>
 *   NET_SOCK_BUCKET_SHIFT.  A per-socket critical section takes the one bucket
 *   that owns the socket; a section that touches two sockets takes their two
 *   buckets in ascending bucket order, so no pair is ever held in two
 *   directions and the order is acyclic.  At most two bucket locks are held at
 *   any time.
 * - Never hold more than one bucket lock while walking the table: taking all
 *   NET_SOCK_BUCKETS with interrupts disabled livelocks against an interrupt
 *   that reaches a lookup on a lock its own interrupted context was holding
 *   (see the same failure recorded for VFS dcache in kernel/fs/vfs/dcache.c).
 *   net_socket_table_walk() and every other table scan walk bucket by bucket,
 *   taking and releasing one bucket at a time.
 * - g_lwip_lock is never held together with any net bucket lock.  This is the
 *   unchanged meaning of the old "g_lwip_lock and g_net_lock are never held
 *   together" rule (docs/net/network-lock-contract.md), and it is now true
 *   everywhere rather than "true except on the accept path".
 * - A socket with reg_idx < 0 (being created, or racing a concurrent close)
 *   has no bucket.  Such accesses take net_bucket[NET_SOCK_ORPHAN_BUCKET],
 *   re-check s->in_registry, and bail out if it is clear.
 * - net_register_socket_locked() takes shard locks itself, one at a time, so
 *   it must NOT be called with another socket's bucket lock held.  Callers
 *   resolve their counterpart (listener, peer) through a search that returns a
 *   reference, register with no lock held, and only then take the ordered
 *   pair.  This is the one rule that is easy to break by accident: every new
 *   accept/connect path has to be written in that shape.
 * - Carrying a net_socket_t pointer out of a bucket critical section requires
 *   net_socket_ref() / net_socket_free().  The bucket lock can no longer be
 *   what keeps the object alive, because a lookup that finds a socket in one
 *   bucket and then needs a second bucket cannot keep the first one held.
 * - Bucket locks are independent of the task locks above: no net bucket lock
 *   nests under tasklist_lock / park_lock / runq_lock, and none of those three
 *   is ever taken while a bucket lock is held.
 * - Never acquire a task's park_lock while holding a runqueue lock (INV-P4b).
 *   A local scheduler pick is runqueue-only: it publishes ->dispatching and
 *   ->owner_cpu under that one CPU's runqueue lock and releases the lock before
 *   the switch path takes the selected task's park_lock.  Enqueue, migrate,
 *   unpick, Park, exit and reap go the other way and nest park_lock ->
 *   runq_lock.  A park_lock holder therefore cannot trust ->dispatching /
 *   ->owner_cpu and must read them atomically.
 * - When two different tasks' park_locks must be held at the same time, take
 *   them in ascending task-pointer order and release them in reverse.  Every
 *   site that does this goes through proc_lock_two_tasks() /
 *   proc_unlock_two_tasks() so the order is written down once.  Per-CPU slots
 *   (g_cpu_switch_out[]) sit outside that order.
 * - Never block while holding a spinlock or while interrupts are disabled.
 * - Do not call into VFS, memory allocation, or scheduler paths while holding a
 *   device or lwIP lock unless the callee is documented nonblocking.
 * - New locks must either fit this order or document a narrower local order in
 *   docs/drivers/guide/lock-order.md before use.
 */

typedef struct spinlock {
    volatile int locked;
    /* Holder identity for [LOCK-STALL] and the owner == cur self-deadlock
     * test.  Written only under CONFIG_DEBUG_LOCKS, and only by an acquire
     * that actually contended, so NULL here does not mean the lock is free:
     * read it only inside a stall report. */
    void *owner;
    uintptr_t owner_ra;
    const char *name;   /* debug name, NULL unless spin_set_debug() */
    void *container;
    /* Contention telemetry: incremented only when an acquire actually finds
     * the lock held, so the uncontended fast path is unchanged.  Read via
     * /proc/a20/lock_contention. */
    uint64_t contended_acquires;
    uint64_t contended_spins;
    uint64_t contended_max_spins;
    /* Non-NULL only for locks registered for callsite sampling (see
     * lock_counters_enable_callsite()); the contended path records the
     * caller's return address so the audit can attribute contention. */
    struct lock_callsite_sample *samples;
} spinlock_t;

/* Per-callsite contention record.  Only the contended path touches the sample
 * table, so uncontended acquires never pay for it. */
#define LOCK_CALLSITE_SAMPLES 32
typedef struct lock_callsite_sample {
    uintptr_t ra;
    uint64_t contended;
    uint64_t spins;
    uint64_t max_spins;
} lock_callsite_sample_t;

#define SPINLOCK_INIT { 0, NULL, 0, NULL, NULL, 0, 0, 0, NULL }

/* GCC/clang expose no atomic fetch-max builtin, so a monotonic max needs a CAS
 * loop.  Contended path only, matching the counter discipline above. */
static inline void spin_atomic_max(uint64_t *slot, uint64_t value) {
    uint64_t cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (cur < value) {
        if (__atomic_compare_exchange_n(slot, &cur, value, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
    }
}

static inline void spin_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->owner = NULL;
    lock->owner_ra = 0;
    lock->name = NULL;
    lock->container = NULL;
    lock->contended_acquires = 0;
    lock->contended_spins = 0;
    lock->contended_max_spins = 0;
    lock->samples = NULL;
}

static inline void spin_set_debug(spinlock_t *lock, const char *name, void *container) {
    if (!lock)
        return;
    lock->name = name;
    lock->container = container;
}

static inline void spin_lock_at(spinlock_t *lock, uintptr_t caller_ra) {
    uint64_t spins = 0;
    uint64_t stall_start = 0;
    uint64_t next_report = 0;
    int armed = 0;
    struct task_t *cur = NULL;
    uintptr_t waiter_ra = caller_ra ? caller_ra
                                    : (uintptr_t)__builtin_return_address(0);
    /* The exchange must be retried after the owner releases the lock: an
     * acquire that waits and then falls through without re-acquiring would
     * enter the critical section unlocked (two CPUs both believing they hold
     * the lock).  The counters are only touched when the lock is contended,
     * so the uncontended fast path stays a single exchange. */
    /* Resolve the call-site slot before waiting.  The spin count is only known
     * once the inner loop drains, and it has to land on the same slot the
     * acquire is charged to; resolving it up front is what lets the audit show
     * per-site spin cost instead of a column that merely repeats the acquires. */
    lock_callsite_sample_t *site = NULL;
    if (lock->samples) {
        unsigned slot = (unsigned)(((waiter_ra >> 4) ^ (waiter_ra >> 20)) &
                                   (LOCK_CALLSITE_SAMPLES - 1));
        site = &lock->samples[slot];
        if (__atomic_load_n(&site->ra, __ATOMIC_RELAXED) != waiter_ra) {
            uintptr_t expect = 0;
            __atomic_compare_exchange_n(&site->ra, &expect, waiter_ra, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
        if (__atomic_load_n(&site->ra, __ATOMIC_RELAXED) != waiter_ra)
            site = NULL;
    }
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&lock->contended_acquires, 1, __ATOMIC_RELAXED);
        if (site)
            __atomic_fetch_add(&site->contended, 1, __ATOMIC_RELAXED);
        uint64_t spun_before = spins;
        /* The stall clock and the reporting identity are read on the first
         * contended observation, not before the exchange: timer_get_ticks()
         * costs rdtsc plus two 64-bit divisions and proc_current() costs a
         * cpuid on SMP x86, and neither value means anything until this CPU
         * has actually failed to take the lock. */
        if (!armed) {
            armed = 1;
            stall_start = timer_get_ticks();
            next_report = stall_start + MS_TO_TICKS(5000);
            cur = proc_current();
        }
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            if ((++spins & ((1UL << 20) - 1)) == 0) {
                uint64_t now = timer_get_ticks();
                uint64_t elapsed = now - stall_start;
                if (now >= next_report) {
                    struct task_t *owner = (struct task_t *)lock->owner;
                    printf("[LOCK-STALL] cpu=%u lock=%p name=%s waiter=%d owner=%d owner_ra=0x%lx waiter_ra=0x%lx spins=%lu elapsed_ms=%lu\n",
                           cpu_current_id(), (void *)lock,
                           lock->name ? lock->name : "?",
                           cur ? proc_task_pid(cur) : -1,
                           owner ? proc_task_pid(owner) : -1,
                           (unsigned long)lock->owner_ra,
                           (unsigned long)waiter_ra, spins,
                           (unsigned long)(elapsed * 1000 / TICKS_PER_SEC));
                    if (owner == cur) {
                        extern void kallsyms_print(uint64_t addr);
                        struct backtrace_frame frames[32];
                        uint64_t fp =
                            (uint64_t)(uintptr_t)__builtin_frame_address(0);
                        int n = arch_unwind_frames(fp, frames, 32);
                        printf("  self-deadlock backtrace irq=%d (%d frames):\n",
                               arch_irqs_enabled() ? 1 : 0, n);
                        for (int i = 0; i < n && i < 32; i++) {
                            printf("    [%d] pc=0x%lx ", i,
                                   (unsigned long)frames[i].pc);
                            kallsyms_print(frames[i].pc);
                            printf("\n");
                        }
                    }
                    next_report = now + MS_TO_TICKS(5000);
                }
            }
            arch_cpu_relax();
        }
        if (site)
            __atomic_fetch_add(&site->spins, spins - spun_before,
                               __ATOMIC_RELAXED);
    }
    if (spins) {
        __atomic_fetch_add(&lock->contended_spins, spins, __ATOMIC_RELAXED);
        spin_atomic_max(&lock->contended_max_spins, spins);
        if (site)
            spin_atomic_max(&site->max_spins, spins);
    }
    #if CONFIG_DEBUG_LOCKS
    /* The owner is diagnostic state: it feeds nothing but [LOCK-STALL]'s
     * reporting identity and the owner == cur self-deadlock test, and only
     * once a lock has already spun for five seconds.  Recording it costs a
     * store on every acquire at all ~920 call sites, so it stays behind
     * CONFIG_DEBUG_LOCKS -- the same choice spin_trylock_irqsave() makes.
     * Left unconditional it would be wrong as well as slow: cur is only read
     * once this CPU has actually failed to take the lock, so an uncontended
     * acquire would publish NULL and erase the very holder the stall report
     * is trying to name. */
    lock->owner = cur;
    lock->owner_ra = waiter_ra;
#endif
}

static inline void spin_lock(spinlock_t *lock) {
    spin_lock_at(lock, (uintptr_t)__builtin_return_address(0));
}

static inline void spin_unlock(spinlock_t *lock) {
    lock->owner = NULL;
    lock->owner_ra = 0;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    spin_lock_at(lock, (uintptr_t)__builtin_return_address(0));
    return flags;
}

/*
 * Non-blocking variant used by the idle-task steal path.  On success the lock
 * is held with interrupts disabled and *flags must be passed to
 * spin_unlock_irqrestore(); on failure interrupts are restored and 0 returned.
 */
static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags) {
    uint64_t f = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        if (f)
            arch_local_irq_enable();
        return 0;
    }
#if CONFIG_DEBUG_LOCKS
    lock->owner = proc_current();
    lock->owner_ra = (uintptr_t)__builtin_return_address(0);
#endif
    *flags = f;
    return 1;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags)
        arch_local_irq_enable();
}

#endif /* _CORE_LOCK_H */
