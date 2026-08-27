#include "core/futex.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/timekeeping.h"
#include "proc/proc.h"
#include "proc/park.h"
#include "proc/signal.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/proc_internal.h"
#include <stdint.h>
#include "mm/slab.h"
#include "sys/usercopy.h"

/*
 * A20OS user futex engine (ABI-agnostic core module).
 *
 * Both ABI layers build on this: the Linux ABI translates futex(2) commands
 * onto the calls below, and the Native sync partition (0x0B00) wraps
 * futex_wait_user_ns / futex_wake_user.  proc/exit.c consumes
 * exit_robust_list on task teardown.
 *
 * Waiters are heap-allocated nodes carrying a wait_queue_entry_t, chained on
 * per-bucket wait_queue_t instances (the shared core/sync.h wait-queue
 * abstraction also used by EventQ, channels, pipes, sockets and mutexes; the
 * generic layer only does queue/link/match/wake, never policy).  The bucket
 * lock (now inside the wait_queue) replaces the former single global futex
 * lock, so independent futex words in different buckets never serialize;
 * there is no fixed waiter ceiling.
 *
 * Locking: mm->lock -> bucket lock (kept from the previous design), and for
 * operations touching two futex words the two buckets are locked in address
 * (bucket index) order, so REQUEUE/WAKE_OP cannot deadlock across buckets.
 * proc_try_wake() is always called after dropping the bucket locks, using
 * wait_seq to reject stale wakeups.
 */

#define FUTEX_BUCKETS      64
#define FUTEX_WAKE_BATCH   64

typedef struct futex_wait_key {
    uintptr_t vkey;
    uintptr_t pkey;
    mm_struct_t *mm;
    uint32_t bitset;
} futex_wait_key_t;

typedef struct futex_node {
    wait_queue_entry_t entry;
    futex_wait_key_t key;
} futex_node_t;

typedef struct futex_wake_arg {
    mm_struct_t *mm;
    uintptr_t vkey;
    uintptr_t pkey;
    uint32_t bitset;
} futex_wake_arg_t;

typedef struct futex_requeue_arg {
    uintptr_t vkey2;
    uintptr_t pkey2;
    mm_struct_t *mm;
} futex_requeue_arg_t;

static wait_queue_t g_futex_buckets[FUTEX_BUCKETS] = {
    [0 ... (FUTEX_BUCKETS - 1)] = WAIT_QUEUE_INIT
};

/*
 * Hash on the virtual address only, not the mm pointer: fork-inherited
 * MAP_SHARED mappings keep the same virtual address in parent and child, so
 * cross-process shared futexes land in the same bucket (the pkey match in
 * futex_wake_match then disambiguates within the bucket).  Shared mappings
 * at different virtual addresses in two processes are a documented
 * limitation (same as a virtual-address futex key).
 */
static unsigned futex_bucket_index(uintptr_t vkey)
{
    uint64_t h = (uint64_t)vkey * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 33;
    return (unsigned)(h & (FUTEX_BUCKETS - 1));
}

int futex_timeout_ticks(void *timeout, int absolute, int realtime,
                               uint64_t *ticks_out)
{
    *ticks_out = 0;
    if (!timeout) return 0;
    int64_t ts[2];
    if (copy_from_user(ts, timeout, sizeof(ts)) < 0) return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) return -EINVAL;

    if (!absolute) {
        uint64_t ticks = (uint64_t)ts[0] * TICKS_PER_SEC +
                         ((uint64_t)ts[1] * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
        *ticks_out = ticks ? ticks : 1;
        return 0;
    }

    uint64_t now_ts[2];
    if (realtime) timekeeping_get_realtime(now_ts);
    else timekeeping_get_monotonic(now_ts);
    if ((uint64_t)ts[0] < now_ts[0] ||
        ((uint64_t)ts[0] == now_ts[0] && (uint64_t)ts[1] <= now_ts[1]))
        return -ETIMEDOUT;

    uint64_t sec = (uint64_t)ts[0] - now_ts[0];
    uint64_t nsec;
    if ((uint64_t)ts[1] >= now_ts[1]) {
        nsec = (uint64_t)ts[1] - now_ts[1];
    } else {
        sec--;
        nsec = 1000000000ULL + (uint64_t)ts[1] - now_ts[1];
    }
    uint64_t ticks = sec * TICKS_PER_SEC +
                     (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
    *ticks_out = ticks ? ticks : 1;
    return 0;
}

static uintptr_t futex_phys_key(int *uaddr)
{
    task_t *t = proc_current();
    if (!t || !t->pgdir || !uaddr) return (uintptr_t)uaddr;
    paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)uaddr);
    return pa ? (uintptr_t)pa : 0;
}

/*
 * FUTEX_WAIT_RECHECK_PROTOCOL
 *
 * The first copy_from_user() below may fault the page in.  The actual
 * wait-side linearization point is this non-faulting load while mm->lock and
 * the bucket lock are both held: munmap cannot invalidate the translation,
 * and a matching FUTEX_WAKE cannot inspect the bucket until the waiter is
 * linked.
 */
static int futex_user_load_locked(task_t *task, int *uaddr, int *value,
                                  uintptr_t *pkey)
{
    if (!task || !task->mm || !uaddr || !value || !pkey)
        return -EFAULT;

#ifdef CONFIG_NOMMU
    *value = __atomic_load_n(uaddr, __ATOMIC_ACQUIRE);
    *pkey = (uintptr_t)uaddr;
    return 0;
#else
    if (!task->pgdir)
        return -EFAULT;
    paddr_t pa = pt_translate(task->pgdir, (vaddr_t)(uintptr_t)uaddr);
    if (!pa)
        return -EFAULT;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -EFAULT;
    volatile int *word =
        (volatile int *)((uintptr_t)pfn_to_virt(pfn) +
                         (pa & (PAGE_SIZE - 1)));
    *value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    *pkey = (uintptr_t)pa;
    return 0;
#endif
}

/*
 * Shared predicate: matches a queued futex node against a (mm, vkey, pkey,
 * bitset) wake argument.  Physical-key equality is what makes cross-process
 * shared futexes wakeable even when their virtual addresses differ.
 */
static bool futex_wake_match(const wait_queue_entry_t *entry, void *arg)
{
    const futex_wait_key_t *w = (const futex_wait_key_t *)entry->priv;
    const futex_wake_arg_t *a = (const futex_wake_arg_t *)arg;
    if (!w || !a)
        return false;
    if (entry->task &&
        (entry->task->state == PROC_UNUSED || entry->task->state == PROC_ZOMBIE))
        return false;
    if (!(w->bitset & a->bitset))
        return false;
    if (w->mm == a->mm && w->vkey == a->vkey)
        return true;
    if (a->pkey && w->pkey == a->pkey)
        return true;
    return false;
}

static void futex_requeue_rekey(wait_queue_entry_t *entry, void *arg)
{
    const futex_requeue_arg_t *r = (const futex_requeue_arg_t *)arg;
    futex_wait_key_t *w = (futex_wait_key_t *)entry->priv;
    if (!w || !r)
        return;
    entry->key = r->vkey2;
    w->vkey = r->vkey2;
    w->pkey = r->pkey2;
    w->mm = r->mm;
}

/* ticks == 0 waits indefinitely. */
int futex_wait_ticks(int *uaddr, int expected, uint64_t ticks, uint32_t bitset)
{
    if (!uaddr) return -EFAULT;
    if ((uintptr_t)uaddr & (sizeof(int) - 1)) return -EINVAL;
    if (bitset == 0) return -EINVAL;

    task_t *t = proc_current();
    if (!t) return -ESRCH;

    int uval;
    if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) return -EFAULT;
    if (uval != expected) return -EAGAIN;

    uint64_t until = ticks ? timer_get_ticks() + ticks : 0;
    uintptr_t vkey = (uintptr_t)uaddr;
    uintptr_t pkey = 0;

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, until);
    if (!token.task)
        return -EAGAIN;

    unsigned bucket = futex_bucket_index(vkey);
    wait_queue_t *q = &g_futex_buckets[bucket];
    uint64_t mm_flags = spin_lock_irqsave(&t->mm->lock);
    uint64_t flags = spin_lock_irqsave(&q->lock);
    int load_ret = futex_user_load_locked(t, uaddr, &uval, &pkey);
    if (load_ret < 0 || uval != expected) {
        spin_unlock_irqrestore(&q->lock, flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return load_ret < 0 ? load_ret : -EAGAIN;
    }
    if (signal_task_has_unblocked(t)) {
        spin_unlock_irqrestore(&q->lock, flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return -ERESTARTSYS;
    }

    futex_node_t *node = kmalloc(sizeof(*node));
    if (!node) {
        spin_unlock_irqrestore(&q->lock, flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return -ENOMEM;
    }
    memset(node, 0, sizeof(*node));
    node->key.vkey = vkey;
    node->key.pkey = pkey;
    node->key.mm = t->mm;
    node->key.bitset = bitset;
    node->entry.priv = &node->key;

    /* The park protocol allows one park at a time, so at most one stale node
     * of this task can linger here; drop it defensively before linking. */
    (void)wait_queue_purge_task_locked(q, t);
    if (!wait_queue_link_locked(q, &node->entry, token, vkey)) {
        spin_unlock_irqrestore(&q->lock, flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        kfree(node);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return -ESRCH;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    spin_unlock_irqrestore(&t->mm->lock, mm_flags);

    proc_wake_reason_t reason = proc_park_commit(token);
    proc_park_finish(token);

    wait_queue_unlink(q, &node->entry);
    kfree(node);

    if (proc_wake_reason_is_task_interrupt(reason) ||
        signal_task_has_unblocked(t))
        return -ERESTARTSYS;
    if (reason == PROC_WAKE_TIMEOUT)
        return -ETIMEDOUT;
    return 0;
}

int futex_wait(int *uaddr, int expected, void *timeout, uint32_t bitset,
                         int absolute_timeout, int realtime_timeout)
{
    uint64_t ticks = 0;
    int tr = futex_timeout_ticks(timeout, absolute_timeout, realtime_timeout, &ticks);
    if (tr < 0) return tr;
    return futex_wait_ticks(uaddr, expected, ticks, bitset);
}

/* Native ABI entry point: relative timeout in nanoseconds,
 * FUTEX_NS_INFINITE (UINT64_MAX) waits indefinitely. */
int futex_wait_user_ns(int *uaddr, int expected, uint64_t timeout_ns)
{
    uint64_t ticks = 0;
    if (timeout_ns != UINT64_MAX) {
        uint64_t sec = timeout_ns / 1000000000ULL;
        uint64_t nsec = timeout_ns % 1000000000ULL;
        uint64_t now = timer_get_ticks();
        if (sec > (UINT64_MAX - now) / TICKS_PER_SEC) {
            ticks = UINT64_MAX - now; /* saturate: effectively infinite */
        } else {
            ticks = sec * TICKS_PER_SEC +
                    (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
            if (!ticks) ticks = 1;
        }
    }
    return futex_wait_ticks(uaddr, expected, ticks, FUTEX_BITSET_MATCH_ANY);
}

int futex_wake(int *uaddr, int nr, uint32_t bitset, int private)
{
    task_t *cur = proc_current();
    if (!uaddr) return -EFAULT;
    if (bitset == 0) return -EINVAL;
    if (nr < 0) return -EINVAL;

    uintptr_t vkey = (uintptr_t)uaddr;
    /*
     * PRIVATE futexes key only on (mm, vaddr); the physical address is used
     * solely for cross-process SHARED matching, so the page-table walk in
     * futex_phys_key() is pure overhead for them (cargo's jobserver and most
     * libc locks are PRIVATE).  Passing pkey=0 also stops a PRIVATE wake from
     * ever matching a foreign mm by physical page alias.
     */
    uintptr_t pkey = private ? 0 : futex_phys_key(uaddr);
    mm_struct_t *mm = cur ? cur->mm : NULL;
    wait_queue_t *q = &g_futex_buckets[futex_bucket_index(vkey)];
    futex_wake_arg_t arg = { mm, vkey, pkey, bitset };

    int woke = 0;
    while (woke < nr) {
        unsigned want = (unsigned)(nr - woke);
        if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        bool complete = false;
        unsigned got = wait_queue_collect_matching(q, futex_wake_match, &arg,
                                                   want, PROC_WAKE_EVENT,
                                                   &wake_q, &complete);
        if (got == 0)
            break;
        (void)proc_wake_q_flush(&wake_q);
        woke += (int)got;
        if (complete || got < want)
            break;
    }
    return woke;
}

int futex_wake_user(int *uaddr, int nr)
{
    return futex_wake(uaddr, nr, FUTEX_BITSET_MATCH_ANY, 0);
}

int futex_requeue(int *uaddr, int wake_nr, int requeue_nr, int *uaddr2,
                         int check_cmp, int cmpval, int private)
{
    if (!uaddr || !uaddr2) return -EFAULT;
    if (wake_nr < 0 || requeue_nr < 0) return -EINVAL;
    if (check_cmp) {
        int uval;
        if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) return -EFAULT;
        if (uval != cmpval) return -EAGAIN;
    }

    task_t *cur = proc_current();
    mm_struct_t *mm = cur ? cur->mm : NULL;
    uintptr_t vkey1 = (uintptr_t)uaddr;
    uintptr_t pkey1 = private ? 0 : futex_phys_key(uaddr);
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t pkey2 = private ? 0 : futex_phys_key(uaddr2);
    unsigned b1 = futex_bucket_index(vkey1);
    unsigned b2 = futex_bucket_index(vkey2);
    wait_queue_t *q1 = &g_futex_buckets[b1];
    wait_queue_t *q2 = &g_futex_buckets[b2];

    int done = 0;

    /* Wake phase on bucket1 (q1 lock). */
    futex_wake_arg_t arg1 = { mm, vkey1, pkey1, FUTEX_BITSET_MATCH_ANY };
    while (done < wake_nr) {
        unsigned want = (unsigned)(wake_nr - done);
        if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned got = wait_queue_collect_matching(q1, futex_wake_match, &arg1,
                                                   want, PROC_WAKE_EVENT,
                                                   &wake_q, NULL);
        if (got == 0)
            break;
        (void)proc_wake_q_flush(&wake_q);
        done += (int)got;
    }

    /* Requeue phase: remaining bucket1 matches move to bucket2. */
    int moved = 0;
    if (requeue_nr > 0) {
        futex_wake_arg_t argr = { mm, vkey1, pkey1, FUTEX_BITSET_MATCH_ANY };
        futex_requeue_arg_t rarg = { vkey2, pkey2, mm };
        moved = (int)wait_queue_requeue_matching(q1, q2, futex_wake_match,
                                                 &argr,
                                                 (unsigned)requeue_nr,
                                                 futex_requeue_rekey,
                                                 &rarg);
    }
    return done + moved;
}

static int futex_wake_op_cmp(int oldval, int cmp, int cmparg)
{
    switch (cmp) {
    case FUTEX_OP_CMP_EQ: return oldval == cmparg;
    case FUTEX_OP_CMP_NE: return oldval != cmparg;
    case FUTEX_OP_CMP_LT: return oldval < cmparg;
    case FUTEX_OP_CMP_LE: return oldval <= cmparg;
    case FUTEX_OP_CMP_GT: return oldval > cmparg;
    case FUTEX_OP_CMP_GE: return oldval >= cmparg;
    default: return 0;
    }
}

static int futex_wake_op_new_value(int oldval, int op, int oparg, int *out)
{
    switch (op & 0xf) {
    case FUTEX_OP_SET:  *out = oparg; return 0;
    case FUTEX_OP_ADD:  *out = oldval + oparg; return 0;
    case FUTEX_OP_OR:   *out = oldval | oparg; return 0;
    case FUTEX_OP_ANDN: *out = oldval & ~oparg; return 0;
    case FUTEX_OP_XOR:  *out = oldval ^ oparg; return 0;
    default: return -EINVAL;
    }
}

int futex_wake_op(int *uaddr, int wake_nr, int wake2_nr,
                         int *uaddr2, int encoded_op, int private)
{
    if (!uaddr || !uaddr2) return -EFAULT;
    if (wake_nr < 0 || wake2_nr < 0) return -EINVAL;

    int oldval;
    if (copy_from_user(&oldval, uaddr2, sizeof(oldval)) < 0)
        return -EFAULT;

    int op = (encoded_op >> 28) & 0xf;
    int cmp = (encoded_op >> 24) & 0xf;
    int oparg = (encoded_op >> 12) & 0xfff;
    int cmparg = encoded_op & 0xfff;
    if (op & FUTEX_OP_OPARG_SHIFT) {
        op &= ~FUTEX_OP_OPARG_SHIFT;
        if (oparg >= 31) return -EINVAL;
        oparg = 1 << oparg;
    }

    int newval;
    int vr = futex_wake_op_new_value(oldval, op, oparg, &newval);
    if (vr < 0) return vr;
    if (copy_to_user(uaddr2, &newval, sizeof(newval)) < 0)
        return -EFAULT;

    task_t *cur = proc_current();
    mm_struct_t *mm = cur ? cur->mm : NULL;
    uintptr_t vkey1 = (uintptr_t)uaddr;
    uintptr_t pkey1 = private ? 0 : futex_phys_key(uaddr);
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t pkey2 = private ? 0 : futex_phys_key(uaddr2);
    wait_queue_t *q1 = &g_futex_buckets[futex_bucket_index(vkey1)];
    wait_queue_t *q2 = &g_futex_buckets[futex_bucket_index(vkey2)];

    int woke = 0;
    futex_wake_arg_t arg1 = { mm, vkey1, pkey1, FUTEX_BITSET_MATCH_ANY };
    while (woke < wake_nr) {
        unsigned want = (unsigned)(wake_nr - woke);
        if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned got = wait_queue_collect_matching(q1, futex_wake_match, &arg1,
                                                   want, PROC_WAKE_EVENT,
                                                   &wake_q, NULL);
        if (got == 0)
            break;
        (void)proc_wake_q_flush(&wake_q);
        woke += (int)got;
    }

    if (futex_wake_op_cmp(oldval, cmp, cmparg)) {
        int woke2 = 0;
        futex_wake_arg_t arg2 = { mm, vkey2, pkey2, FUTEX_BITSET_MATCH_ANY };
        while (woke2 < wake2_nr) {
            unsigned want = (unsigned)(wake2_nr - woke2);
            if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            unsigned got = wait_queue_collect_matching(q2, futex_wake_match,
                                                       &arg2, want,
                                                       PROC_WAKE_EVENT,
                                                       &wake_q, NULL);
            if (got == 0)
                break;
            (void)proc_wake_q_flush(&wake_q);
            woke += (int)got;
            woke2 += (int)got;
        }
    }
    return woke;
}

/*
 * PI futex protocol (FUTEX_LOCK_PI / UNLOCK_PI / TRYLOCK_PI).
 *
 * The futex word holds (tid & FUTEX_TID_MASK) | FUTEX_WAITERS |
 * FUTEX_OWNER_DIED.  The compare-and-swap on the user word is performed
 * against the kernel-mapped page while the hash-bucket lock is held, so
 * concurrent futex operations on the same word (all hashing to the same
 * bucket) serialize on the read-modify-write.
 *
 * Priority inheritance is expressed in EEVDF terms as weight donation:
 * while a waiter parks on a PI futex, the owner's effective weight is
 * raised to at least the waiter's (task_t::pi_boost_weight, consulted by
 * eevdf_weight), so the owner's vruntime accrues at the waiter's rate and
 * it stays eligible ahead of same-weight competitors.  UNLOCK_PI clears
 * the donation; an owner holding several contended PI futexes loses the
 * donations of locks it still holds (documented approximation -- Linux
 * tracks per-futex pi_state chains; chained PI walk is out of scope).
 * Waiters re-donate whenever they re-block, which retargets the boost
 * across owner handoff.
 */

/* Map the user futex word to a kernel virtual address, pinning the page
 * translation.  Caller must hold mm->lock. */
static int futex_user_word_map(task_t *task, int *uaddr, volatile int **word,
                               uintptr_t *pkey_out)
{
    if (!task || !task->mm || !task->pgdir || !uaddr || !word)
        return -EFAULT;
#ifdef CONFIG_NOMMU
    *word = uaddr;
    if (pkey_out)
        *pkey_out = (uintptr_t)uaddr;
    return 0;
#else
    paddr_t pa = pt_translate(task->pgdir, (vaddr_t)(uintptr_t)uaddr);
    if (!pa)
        return -EFAULT;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -EFAULT;
    *word = (volatile int *)((uintptr_t)pfn_to_virt(pfn) +
                             (pa & (PAGE_SIZE - 1)));
    if (pkey_out)
        *pkey_out = (uintptr_t)pa;
    return 0;
#endif
}

int futex_pi_acquire(int *uaddr, int try_only)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint32_t tid = (uint32_t)t->pid;
    uintptr_t vkey = (uintptr_t)uaddr;
    wait_queue_t *q = &g_futex_buckets[futex_bucket_index(vkey)];

    for (;;) {
        uint64_t mm_flags = spin_lock_irqsave(&t->mm->lock);
        uint64_t q_flags = spin_lock_irqsave(&q->lock);

        volatile int *word = NULL;
        uintptr_t pkey = 0;
        int mr = futex_user_word_map(t, uaddr, &word, &pkey);
        if (mr < 0) {
            spin_unlock_irqrestore(&q->lock, q_flags);
            spin_unlock_irqrestore(&t->mm->lock, mm_flags);
            return mr;
        }
        int oldv = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if ((oldv & FUTEX_TID_MASK) == 0 &&
            (oldv == 0 || (oldv & FUTEX_OWNER_DIED))) {
            /* Free or owner-died: claim, preserving OWNER_DIED so the
             * acquirer can detect the inconsistent state (Linux). */
            int want = (int)tid | (oldv & (FUTEX_OWNER_DIED | FUTEX_WAITERS));
            if (__atomic_compare_exchange_n(word, &oldv, want, 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                spin_unlock_irqrestore(&q->lock, q_flags);
                spin_unlock_irqrestore(&t->mm->lock, mm_flags);
                return 0;
            }
            spin_unlock_irqrestore(&q->lock, q_flags);
            spin_unlock_irqrestore(&t->mm->lock, mm_flags);
            continue;
        }
        if ((uint32_t)(oldv & FUTEX_TID_MASK) == tid) {
            spin_unlock_irqrestore(&q->lock, q_flags);
            spin_unlock_irqrestore(&t->mm->lock, mm_flags);
            return -EDEADLK;
        }
        if (try_only) {
            spin_unlock_irqrestore(&q->lock, q_flags);
            spin_unlock_irqrestore(&t->mm->lock, mm_flags);
            return -EAGAIN;
        }
        /* Claimed by another task: publish FUTEX_WAITERS and park. */
        if (!(oldv & FUTEX_WAITERS)) {
            int want = oldv | FUTEX_WAITERS;
            __atomic_compare_exchange_n(word, &oldv, want, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        int wait_val = oldv | FUTEX_WAITERS;
        spin_unlock_irqrestore(&q->lock, q_flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);

        /* Donate our weight to the owner while parked (EEVDF PI).  Safe
         * outside the bucket lock: a stale donation to a just-released
         * owner is cleared by its next UNLOCK_PI. */
        int owner_tid = wait_val & FUTEX_TID_MASK;
        if (owner_tid > 0) {
            task_t *owner = proc_find_get(owner_tid);
            if (owner) {
                proc_sched_pi_boost(owner, t);
                proc_put(owner);
            }
        }

        int wr = futex_wait_ticks(uaddr, wait_val, 0, FUTEX_BITSET_MATCH_ANY);
        if (wr < 0 && wr != -ETIMEDOUT)
            return wr;
    }
}

int futex_pi_release(int *uaddr)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint32_t tid = (uint32_t)t->pid;
    uintptr_t vkey = (uintptr_t)uaddr;
    wait_queue_t *q = &g_futex_buckets[futex_bucket_index(vkey)];

    uint64_t mm_flags = spin_lock_irqsave(&t->mm->lock);
    uint64_t q_flags = spin_lock_irqsave(&q->lock);
    volatile int *word = NULL;
    int mr = futex_user_word_map(t, uaddr, &word, NULL);
    if (mr < 0) {
        spin_unlock_irqrestore(&q->lock, q_flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        return mr;
    }
    int oldv = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if ((oldv & FUTEX_TID_MASK) != tid) {
        spin_unlock_irqrestore(&q->lock, q_flags);
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        return -EPERM;
    }
    int had_waiters = oldv & FUTEX_WAITERS;
    int zero = 0;
    __atomic_store_n(word, zero, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&q->lock, q_flags);
    spin_unlock_irqrestore(&t->mm->lock, mm_flags);

    /* Clear PI weight donation on unlock. */
    proc_sched_pi_unboost(t);

    if (had_waiters)
        futex_wake(uaddr, 1, FUTEX_BITSET_MATCH_ANY, 0);
    return 0;
}

void exit_robust_list(task_t *t)
{
    if (!t || !t->robust_list_head) return;

    struct robust_list_head head;
    if (copy_from_user(&head, (void *)t->robust_list_head, sizeof(head)) < 0)
        return;

    int tid = t->pid;
    int count = 0;
    uintptr_t entry = (uintptr_t)head.list.next;
    uintptr_t head_addr = t->robust_list_head;

    while (entry && entry != head_addr && count < ROBUST_LIST_LIMIT) {
        uintptr_t futex_addr = entry + (uintptr_t)head.futex_offset;
        uint32_t futex_word = 0;
        if (copy_from_user(&futex_word, (void *)futex_addr, sizeof(futex_word)) < 0)
            goto next;
        if ((futex_word & FUTEX_TID_MASK) == (uint32_t)tid) {
            uint32_t new_val = (futex_word & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
            copy_to_user((void *)futex_addr, &new_val, sizeof(new_val));
            if (futex_word & FUTEX_WAITERS)
                futex_wake_user((int *)futex_addr, 1);
        }
next:
        struct robust_list node;
        if (copy_from_user(&node, (void *)entry, sizeof(node)) < 0)
            break;
        entry = (uintptr_t)node.next;
        count++;
    }

    t->robust_list_head = 0;
}
