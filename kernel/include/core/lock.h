#ifndef _CORE_LOCK_H
#define _CORE_LOCK_H

#include "core/types.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/preempt.h"

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
 *   lwIP ingress lock -> CT guard (released) -> stage frame -> release ingress
 *                      -> lane/control core processing
 *   lwIP control barrier: ingress/global gate -> all owner lanes ascending
 *   lwIP lane or control core -> keyed shared-state guard -> release before
 *                          taking lane/control/socket/ingress locks
 *   lwIP core -> heap lock -> allocator SYS_ARCH_PROTECT
 *   lwIP TX staging guard -> conntrack/NAT guard (released) -> nonblocking
 *                          driver send callback (leaf; no lwIP reentry)
 *   lwIP core locks are never nested with net_socket_t.lock or net_bucket locks
 *
 * cg_node.lock is never held together with tasklist_lock: the memory
 * controller releases it before scanning tasks for an OOM victim.
 *
 * Rules:
 *
 * Lock-safe network entry points (see docs/net/network-lock-contract.md):
 * - CONFIG_NET_LANES=1 retains the single core lock. With multiple lanes,
 *   a20_lwip_lane_lock() protects ordinary owner-lane traffic and
 *   a20_lwip_lock() is the ingress gate or the outer part of a control barrier
 *   that then takes all lane locks in ascending order.
 * - Ingress only stages frames and releases its gate before lane consumers
 *   process them. Never upgrade a held lane/control lock to ingress or to the
 *   all-lane barrier.
 * - Cross-lane protocol state uses a keyed shared-domain guard. The order is
 *   lane/control -> shared guard; shared guards must not acquire lane/control,
 *   ingress, or socket locks. Allocation under ARP/ND6 guards follows
 *   shared-domain -> heap -> allocator protect, with no reverse edge.
 * - Output serializes a netif's staging frame, takes/releases the conntrack/NAT
 *   guard, then invokes a nonblocking driver send callback. Drivers must not
 *   synchronously re-enter lwIP. lwIP core locks never nest with socket or
 *   socket-table locks; callbacks defer socket work to bottom-halves.
 *
 * Socket locks and socket-table bucket locks (stage E of
 * docs/net/net-lanes.md; full text in docs/net/network-lock-contract.md):
 * - Every per-socket field -- receive and accept queues, rx_count, closed /
 *   shut_rd / shut_wr / peer_closed, local / peer addresses and their lengths,
 *   lane, timeouts, the accept stage -- is protected by net_socket_t.lock and by
 *   nothing else.  net_sock_lock() / net_sock_unlock() are the only way to take
 *   it.
 * - The bucket lock protects the registry slot table and the free bitmap, and
 *   only that: net_register_socket_locked(), net_socket_unregister() and
 *   net_bucket_slot_ref() (which pins one slot and hands back a reference).
 *   Nothing else takes a bucket lock for longer than one slot read.
 * - Order, outermost first:
 *       net_bucket[b]  ->  net_socket_t.lock  ->  (task locks above)
 *     A bucket lock is never taken while a socket lock is held.  That is the
 *     whole ABBA surface now, because a bucket lock is reached only from
 *     net_register_socket_locked(), net_socket_unregister() and
 *     net_bucket_slot_ref(), and each of those is a leaf that the caller
 *     invokes with no net lock held.
 * - A bucket lock and one socket lock may be held together -- that is how
 *   net_bucket_scan() and the four broadcast walks work -- but never two bucket
 *   locks and never a second socket lock underneath.  Two sockets are taken
 *     with net_sock_lock2(), which orders by address, so no pair is ever held in
 *     two directions and the order is acyclic.
 * - Never hold more than one bucket lock while walking the table: taking all
 *   NET_SOCK_BUCKETS with interrupts disabled livelocks against an interrupt
 *   that reaches a lookup on a lock its own interrupted context was holding
 *   (see the same failure recorded for VFS dcache in kernel/fs/vfs/dcache.c).
 *   net_socket_table_walk() and every other table scan walk bucket by bucket,
 *   taking and releasing one bucket at a time.
 * - No lwIP ingress, lane, control, shared-domain, heap, or allocator-protect
 *   lock is held together with any net lock -- neither a socket lock nor a
 *   bucket lock.  This keeps core callbacks from entering socket state while
 *   holding protocol locks; deferred bottom-halves run after core locks drop.
 * - A socket that owns no registry slot (being created, the accepted end of an
 *   AF_UNIX stream, or one whose close() is in flight) needs no special shard.
 *   Its per-socket state is covered by its own lock; only the slot does not
 *   exist, and the slot is the bucket lock's business.
 * - net_register_socket_locked() takes bucket locks itself, one at a time, so
 *   it must NOT be called with a socket lock held.  Callers resolve their
 *   counterpart (listener, peer) through a search that returns a reference,
 *   register with no lock held, and only then take the ordered pair.  This is
 *   the one rule that is easy to break by accident: every new accept/connect
 *   path has to be written in that shape.
 * - net_socket_unregister() takes a bucket lock itself, for the same reason, so
 *   it is likewise called with no net lock held.  Every close path therefore
 *   splits into two phases: mark the socket closed and drain its wait queues
 *   under the socket lock, release it, then release the slot.  Nothing waits for
 *   the reference count to reach zero while a lock is held; the registry's
 *   reference is handed back by the caller after every lock is dropped, because
 *   dropping it can free and obj_cache_free() is not something to run with
 *   interrupts disabled.
 * - Carrying a net_socket_t pointer out of its lock requires net_socket_ref() /
 *   net_socket_free().  The reference is also what pins a peer across the
 *   unlocked window between sampling s->peer and taking the pair.
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
 * - Every spin_lock/spin_trylock_irqsave/spin_unlock bracket is also a
 *   preempt_disable/enable bracket (CONFIG_KERNEL_PREEMPT): a context switch
 *   with the counter raised would inherit someone else's nesting depth, since
 *   the per-CPU counter is only sound because it is provably zero at every
 *   __switch.  context_switch_locked() panics on a nonzero count, so a switch
 *   under a held lock fails loudly instead of corrupting the counter.  Waiting
 *   for a lock does NOT raise the counter -- a spinner stays a legal
 *   switch-out target.  See docs/process-scheduler.md §4.1.
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
    /* Every critical section is a non-preemptible one.  The bump comes after
     * the spin loop so that waiting for the lock does not also disable
     * preemption -- otherwise a task blocked on a lock would never be a legal
     * switch-out target at the IRQ return point. */
    preempt_disable();
}

static inline void spin_lock(spinlock_t *lock) {
    spin_lock_at(lock, (uintptr_t)__builtin_return_address(0));
}

static inline void spin_unlock(spinlock_t *lock) {
    lock->owner = NULL;
    lock->owner_ra = 0;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    /* Last, not first: while the lock is still held the section must still be
     * non-preemptible.  Releasing the counter before the store would let an
     * interrupt that lands in that window reach the IRQ return decision point
     * with preempt == 0 and switch the lock owner out; the next task on this
     * CPU would then spin on a lock whose owner is not running. */
    preempt_enable();
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
    preempt_disable();
    return 1;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags)
        arch_local_irq_enable();
}

#endif /* _CORE_LOCK_H */
