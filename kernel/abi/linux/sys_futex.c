#include "syscall_impl.h"
#include "core/futex.h"
#include "sys/usercopy.h"

/*
 * A20OS Linux ABI futex — wire translation only.
 *
 * The futex engine (hash buckets, wait/wake/requeue/wake_op, PI protocol,
 * robust lists) is an ABI-agnostic core module in kernel/ipc/futex.c; this
 * file only decodes the Linux futex(2) command surface onto it.  The Native
 * sync partition (0x0B00) wraps the same engine via
 * futex_wait_user_ns/futex_wake_user.
 */

int64_t sys_futex(int *uaddr, int op, int val, void *timeout, int *uaddr2, int val3)
{
    int opc = op & FUTEX_CMD_MASK;
    /* FUTEX_CLOCK_REALTIME is only meaningful with FUTEX_WAIT_BITSET; Linux
     * rejects it on every other command (including plain FUTEX_WAIT). */
    if ((op & FUTEX_CLOCK_REALTIME) && opc != FUTEX_WAIT_BITSET)
        return -EINVAL;
    int private = (op & FUTEX_PRIVATE_FLAG) != 0;
    switch (opc) {
    case FUTEX_WAIT:
        return futex_wait(uaddr, val, timeout, FUTEX_BITSET_MATCH_ANY, 0, 0);
    case FUTEX_WAIT_BITSET:
        return futex_wait(uaddr, val, timeout, (uint32_t)val3, 1,
                          (op & FUTEX_CLOCK_REALTIME) != 0);
    case FUTEX_WAKE:
        return futex_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY, private);
    case FUTEX_WAKE_BITSET:
        return futex_wake(uaddr, val, (uint32_t)val3, private);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, val, (int)(intptr_t)timeout, uaddr2, 0, 0,
                             private);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, val, (int)(intptr_t)timeout, uaddr2, 1, val3,
                             private);
    case FUTEX_CMP_REQUEUE_PI:
        /* The requeued waiters surface on uaddr2, then acquire it through
         * their own FUTEX_LOCK_PI call; requeue to the plain bucket is the
         * bounded analogue. */
        return futex_requeue(uaddr, val, (int)(intptr_t)timeout, uaddr2, 1, val3,
                             private);
    case FUTEX_WAKE_OP:
        return futex_wake_op(uaddr, val, (int)(intptr_t)timeout, uaddr2, val3,
                             private);
    case FUTEX_LOCK_PI:
        return futex_pi_acquire(uaddr, 0);
    case FUTEX_TRYLOCK_PI:
        return futex_pi_acquire(uaddr, 1);
    case FUTEX_UNLOCK_PI:
        return futex_pi_release(uaddr);
    case FUTEX_WAIT_REQUEUE_PI: {
        /* Bounded: wait on uaddr for the requeue signal, then acquire the PI
         * futex at uaddr2 in-kernel before returning success. */
        uint64_t ticks = 0;
        int tr = futex_timeout_ticks(timeout, 0, 0, &ticks);
        if (tr < 0) return tr;
        int r = futex_wait_ticks(uaddr, val, ticks, FUTEX_BITSET_MATCH_ANY);
        if (r < 0)
            return r;
        return futex_pi_acquire(uaddr2, 0);
    }
    case FUTEX_FD:
        /* Removed in Linux 5.4; kept for a stable, documented error. */
        return -EINVAL;
    default:
        /* Unknown or reserved commands are refused; no futex command is
         * silently unimplemented. */
        return -EINVAL;
    }
}

/* ---- futex_waitv / futex_requeue (separate Linux syscalls) ---- */

struct futex_waitv_entry {
    uint64_t uaddr;
    uint32_t val;
    uint32_t flags;
};

int64_t sys_futex_waitv(const void *waiters, unsigned nr_futexes,
                        unsigned flags, const void *timeout, uint64_t clockid)
{
    if (flags)
        return -EINVAL;
    if (nr_futexes == 0 || nr_futexes > 128)
        return -EINVAL;
    if (!waiters)
        return -EFAULT;
    (void)clockid;

    struct futex_waitv_entry *ents =
        proc_scratch_buffer(nr_futexes * sizeof(struct futex_waitv_entry));
    if (!ents)
        return -ENOMEM;
    if (copy_from_user(ents, waiters,
                       nr_futexes * sizeof(struct futex_waitv_entry)) < 0)
        return -EFAULT;

    /* Validate all addresses up front so a bad entry fails before sleeping. */
    for (unsigned i = 0; i < nr_futexes; i++) {
        if (!ents[i].uaddr)
            return -EINVAL;
        if (ents[i].flags & ~FUTEX_WAITV_FLAG_BITSET)
            return -EINVAL;
        int32_t v;
        if (copy_from_user(&v, (const void *)(uintptr_t)ents[i].uaddr,
                           sizeof(v)) < 0)
            return -EFAULT;
    }

    uint64_t ticks = 0;
    if (timeout) {
        int tr = futex_timeout_ticks((void *)timeout, 0, 0, &ticks);
        if (tr < 0)
            return tr;
    }

    /* FUTEX_WAITV waits on the first entry whose value matches; it returns
     * the index of the woken futex or -ETIMEDOUT.  With synchronous wait the
     * per-entry semantics are approximated by waiting on the first matching
     * entry and treating mismatches like Linux (skip to next). */
    for (unsigned i = 0; i < nr_futexes; i++) {
        int32_t v;
        if (copy_from_user(&v, (const void *)(uintptr_t)ents[i].uaddr,
                           sizeof(v)) < 0)
            return -EFAULT;
        if (v == (int32_t)ents[i].val) {
            uint32_t bitset = FUTEX_BITSET_MATCH_ANY;
            if (ents[i].flags & FUTEX_WAITV_FLAG_BITSET) {
                bitset = ents[i].flags >> 8;
                if (bitset == 0)
                    return -EINVAL;
            }
            int r = futex_wait_ticks((int *)(uintptr_t)ents[i].uaddr,
                                            (int32_t)ents[i].val, ticks,
                                            bitset);
            if (r < 0)
                return r;
            return (int64_t)i;
        }
    }
    return -EAGAIN;
}

int64_t sys_futex_requeue(int *uaddr, int *uaddr2, int nr_wake, int nr_requeue,
                          uint32_t flags)
{
    /* The standalone futex_requeue syscall is equivalent to the legacy
     * FUTEX_REQUEUE command. */
    return futex_requeue(uaddr, nr_wake, nr_requeue, uaddr2, 0, 0,
                         (flags & FUTEX_PRIVATE_FLAG) != 0);
}

/* futex_wait(2) / futex_wake(2): the split-out syscalls (Linux 6.7+).
 * Equivalent to the legacy FUTEX_WAIT/FUTEX_WAKE commands with a timespec
 * timeout and a 32-bit clockid/flags argument. */

int64_t sys_futex_wait(void *uaddr, uint32_t val, const void *timeout,
                       uint32_t flags)
{
    int realtime = (flags & 1) != 0; /* FUTEX_CLOCK_REALTIME */
    return futex_wait((int *)uaddr, (int)val, (void *)timeout,
                      FUTEX_BITSET_MATCH_ANY, 1, realtime);
}

int64_t sys_futex_wake(void *uaddr, uint32_t nr, uint32_t flags)
{
    return futex_wake((int *)uaddr, (int)nr, FUTEX_BITSET_MATCH_ANY,
                      (flags & FUTEX_PRIVATE_FLAG) != 0);
}
