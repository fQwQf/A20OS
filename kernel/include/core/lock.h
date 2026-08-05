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
 * For the full driver-private lock contracts see docs/drivers/lock-order.md.
 *
 * Global order:
 *   cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
 *   proc_lock -> runq_lock
 *   proc_lock -> signal_state.lock
 *   proc_lock -> files_struct.lock -> VFS global-file/vnode locks
 *   proc_lock -> mm_struct.lock
 *   proc_lock -> a20_handle_table.lock
 *   driver registry/IRQ locks -> device-private locks
 *   g_lwip_lock -> g_net_lock
 *   g_lwip_lock -> virtio-net nonblocking send/recv paths only
 *
 * Rules:
 *
 * Lock-safe network entry points (see docs/network-lock-contract.md):
 * - a20_lwip_lock()/a20_lwip_unlock(): outer lock around all lwIP core calls.
 * - a20_lwip_poll_locked(): progress entry that runs with g_lwip_lock held;
 *   must not allocate, block, or acquire g_net_lock.
 * - a20_lwip_poll(): acquires g_lwip_lock, runs progress, releases it, then
 *   runs the socket bottom-half under g_net_lock only.
 * - lwIP callbacks run under g_lwip_lock and must only stage events into the
 *   preallocated per-PCB ring; allocation, enqueue, and wakeup happen in the
 *   bottom-half with g_net_lock held.
 * - Never acquire proc_lock while holding a runqueue lock. A local scheduler
 *   pick is runqueue-only and releases the runqueue lock before publishing the
 *   selected task under proc_lock.
 * - Never block while holding a spinlock or while interrupts are disabled.
 * - Do not call into VFS, memory allocation, or scheduler paths while holding a
 *   device or lwIP lock unless the callee is documented nonblocking.
 * - New locks must either fit this order or document a narrower local order in
 *   docs/drivers/lock-order.md before use.
 */

typedef struct spinlock {
    volatile int locked;
    void *owner;        /* task_t * while held (always tracked) */
    uintptr_t owner_ra;
    const char *name;   /* debug name, NULL unless spin_set_debug() */
    void *container;
} spinlock_t;

#define SPINLOCK_INIT { 0, NULL, 0, NULL, NULL }

static inline void spin_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->owner = NULL;
    lock->owner_ra = 0;
    lock->name = NULL;
    lock->container = NULL;
}

static inline void spin_set_debug(spinlock_t *lock, const char *name, void *container) {
    if (!lock)
        return;
    lock->name = name;
    lock->container = container;
}

static inline void spin_lock_at(spinlock_t *lock, uintptr_t caller_ra) {
    uint64_t spins = 0;
    uint64_t stall_start = timer_get_ticks();
    uint64_t next_report = MS_TO_TICKS(5000);
    struct task_t *cur = proc_current();
    uintptr_t waiter_ra = caller_ra ? caller_ra
                                    : (uintptr_t)__builtin_return_address(0);
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            if ((++spins & ((1UL << 20) - 1)) == 0) {
                uint64_t elapsed = timer_get_ticks() - stall_start;
                if (elapsed >= next_report) {
                    struct task_t *owner = (struct task_t *)lock->owner;
                    printf("[LOCK-STALL] cpu=%u lock=%p name=%s waiter=%d owner=%d owner_ra=0x%lx waiter_ra=0x%lx spins=%lu elapsed_ms=%lu\n",
                           cpu_current_id(), (void *)lock,
                           lock->name ? lock->name : "?",
                           cur ? proc_task_pid(cur) : -1,
                           owner ? proc_task_pid(owner) : -1,
                           (unsigned long)lock->owner_ra,
                           (unsigned long)waiter_ra, spins,
                           (unsigned long)(elapsed * 1000 / TICKS_PER_SEC));
                    next_report = elapsed + MS_TO_TICKS(5000);
                }
            }
            arch_cpu_relax();
        }
    }
    lock->owner = cur;
    lock->owner_ra = waiter_ra;
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
