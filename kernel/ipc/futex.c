#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
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
#ifdef CONFIG_FLAT_USER_MEMORY
#include "mm/flat_user.h"
#endif
#include "proc/proc_internal.h"
#include <stdint.h>
#include "mm/slab.h"
#include "sys/usercopy.h"
#ifdef CONFIG_FUTEX_COW_WINDOW_TEST
#include "uapi/a20/futex_cow_window.h"
#endif

/*
 * A20OS user futex engine (ABI-agnostic core module).
 *
 * Both ABI layers build on this: the Linux ABI translates futex(2) commands
 * onto the calls below, and the Native sync partition (0x0B00) wraps
 * futex_wait_user_ns / futex_wake_user.  proc/exit.c consumes
 * exit_robust_list on task teardown.
 *
 * Rust owns wait admission, key matching, requeue rekeying, and a stack-scoped
 * waiter whose Drop path unlinks on wake, timeout, signal, or retry. The C
 * bridge retains MM translation pins, bucket/wait-queue locks, and scheduler
 * park mechanics. The shared wait-queue layer is mechanism only.
 *
 * Locking: mm->lock -> bucket lock (kept from the previous design), and for
 * operations touching two futex words the two buckets are locked in address
 * (bucket index) order, so REQUEUE/WAKE_OP cannot deadlock across buckets.
 * proc_try_wake() is always called after dropping the bucket locks, using
 * wait_seq to reject stale wakeups.
 */

#define FUTEX_BUCKETS      64
#define FUTEX_WAKE_BATCH   64

_Static_assert(sizeof(int) == sizeof(uint32_t),
               "futex word and atomic usercopy width must match");

extern int a20_futex_wait_ticks_rust(int *uaddr, int expected, uint64_t ticks,
                                     uint32_t bitset, int private);
extern unsigned a20_futex_bucket_index(uintptr_t vkey, uintptr_t pkey);
extern bool a20_futex_wake_match(const wait_queue_entry_t *entry, void *arg);
extern void a20_futex_requeue_rekey(wait_queue_entry_t *entry, void *arg);
extern unsigned a20_futex_requeue_transaction(unsigned source_index,
                                               unsigned target_index,
                                               void *match_arg,
                                               void *rekey_arg,
                                               unsigned limit,
                                               bool sweep_other_buckets);

typedef struct futex_wait_key {
    uintptr_t vkey;
    uintptr_t pkey;
    mm_struct_t *mm;
    uint32_t bitset;
} futex_wait_key_t;

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

/* Private futexes use a stable virtual key, matched with their mm.  A fork
 * can COW the containing page while another thread is asleep, so a physical
 * key would strand that waiter in the old bucket.  Shared futexes retain a
 * physical key to match peers mapping the same page at different addresses.
 * The ABI must preserve the private flag on both wait and wake operations. */
/*
 * Collect up to @limit matching waiters for a futex keyed in @home, sweeping
 * every other bucket when @may_be_elsewhere.
 *
 * @may_be_elsewhere is set only for a SHARED wake whose own translation of the
 * futex word failed: without a physical key there is no way to know which
 * bucket a peer chose, so the other buckets are the only place it can be.  A
 * wake that did derive the physical key looks in one bucket and stops, and a
 * PRIVATE wake looks in one bucket because both peers share an mm and an
 * address by definition.
 *
 * Each bucket is locked and released one at a time, so this cannot deadlock
 * against wait_queue_requeue_matching(), which takes two bucket locks in
 * address order.
 */
static unsigned futex_collect_below(unsigned home, bool may_be_elsewhere,
                                     wait_queue_match_fn match, void *arg,
                                     unsigned limit, proc_wake_reason_t reason,
                                     proc_wake_q_t *wake_q, bool *complete)
{
    bool drained = true;
    unsigned got = wait_queue_collect_matching(&g_futex_buckets[home], match,
                                               arg, limit, reason, wake_q,
                                               &drained);
    if (got != 0 || !may_be_elsewhere) {
        if (complete)
            *complete = drained;
        return got;
    }

    for (unsigned i = 0; i < FUTEX_BUCKETS && got < limit; i++) {
        if (i == home)
            continue;
        bool bucket_drained = true;
        got += wait_queue_collect_matching(&g_futex_buckets[i], match, arg,
                                           limit - got, reason, wake_q,
                                           &bucket_drained);
        if (!bucket_drained)
            drained = false;
    }
    if (complete)
        *complete = drained;
    return got;
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
 * Map the user futex word to a kernel virtual address and report the physical
 * key the bucket is derived from.  Returns NULL if the word is not mapped.
 *
 * The caller must hold mm->lock, which pins the translation for as long as the
 * mapped pointer is used.
 */
static volatile int *futex_user_word_locked(task_t *task, int *uaddr,
                                            uintptr_t *pkey)
{
    if (!task || !uaddr)
        return NULL;
#ifdef CONFIG_NOMMU
#ifdef CONFIG_FLAT_USER_MEMORY
    if (task->flat_user) {
        if (!a20_flat_range_locked(task->flat_user, (uintptr_t)uaddr,
                                   sizeof(*uaddr)))
            return NULL;
    } else
#endif
    {
        int probe;
        if (copy_from_user(&probe, uaddr, sizeof(probe)) < 0)
            return NULL;
    }
    *pkey = (uintptr_t)uaddr;
    return uaddr;
#else
    if (!task->mm || !task->pgdir)
        return NULL;
    paddr_t pa = pt_translate(task->pgdir, (vaddr_t)(uintptr_t)uaddr);
    if (!pa)
        return NULL;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return NULL;
    *pkey = (uintptr_t)pa;
    return (volatile int *)((uintptr_t)pfn_to_virt(pfn) + (pa & (PAGE_SIZE - 1)));
#endif
}

static uint64_t futex_wait_mapping_lock(task_t *task) {
#ifdef CONFIG_NOMMU
#ifdef CONFIG_FLAT_USER_MEMORY
    if (task->flat_user)
        return spin_lock_irqsave(&task->flat_user->lock);
#endif
    if (!task->mm)
        return 0;
#endif
    return spin_lock_irqsave(&task->mm->lock);
}

static void futex_wait_mapping_unlock(task_t *task, uint64_t flags) {
#ifdef CONFIG_NOMMU
#ifdef CONFIG_FLAT_USER_MEMORY
    if (task->flat_user) {
        spin_unlock_irqrestore(&task->flat_user->lock, flags);
        return;
    }
#endif
    if (!task->mm)
        return;
#endif
    spin_unlock_irqrestore(&task->mm->lock, flags);
}

void *a20_futex_current_task(void)
{
    return proc_current();
}

void *a20_futex_task_mm(void *opaque_task)
{
    task_t *task = opaque_task;
    return task ? task->mm : NULL;
}

int a20_futex_preflight_word(const int *addr, int *out)
{
    if (!out)
        return -EFAULT;
    uint32_t bits;
    if (user_read_u32_atomic((const uint32_t *)(const void *)addr, &bits) < 0)
        return -EFAULT;
    /* Preserve the exact int object representation without a usercopy or an
     * implementation-defined uint32_t-to-int conversion. */
    memcpy(out, &bits, sizeof(bits));
    return 0;
}

int a20_futex_mapping_lock(void *opaque_task, int *addr, int **word,
                           uintptr_t *pkey, uint64_t *flags)
{
    task_t *task = opaque_task;
    if (!task || !word || !pkey || !flags)
        return -EFAULT;
#ifndef CONFIG_NOMMU
    if (!task->mm)
        return -EFAULT;
#endif
    *flags = futex_wait_mapping_lock(task);
    volatile int *mapped = futex_user_word_locked(task, addr, pkey);
    if (!mapped) {
        futex_wait_mapping_unlock(task, *flags);
        return -EFAULT;
    }
    *word = (int *)mapped;
    return 0;
}

void a20_futex_mapping_unlock(void *opaque_task, uint64_t flags)
{
    futex_wait_mapping_unlock(opaque_task, flags);
}

wait_queue_t *a20_futex_bucket(unsigned index)
{
    return &g_futex_buckets[index & (FUTEX_BUCKETS - 1)];
}

uint64_t a20_futex_bucket_lock(wait_queue_t *queue)
{
    return spin_lock_irqsave(&queue->lock);
}

void a20_futex_bucket_unlock(wait_queue_t *queue, uint64_t flags)
{
    spin_unlock_irqrestore(&queue->lock, flags);
}

bool a20_futex_wait_link_locked(wait_queue_t *queue,
                                wait_queue_entry_t *entry,
                                proc_wait_token_t token)
{
    (void)wait_queue_purge_task_locked(queue, token.task);
    return wait_queue_link_locked(queue, entry, token, entry->key);
}

void a20_futex_wait_unlink(wait_queue_t *queue, wait_queue_entry_t *entry)
{
    wait_queue_unlink(queue, entry);
}

unsigned a20_futex_requeue_matching(
    wait_queue_t *source, wait_queue_t *target, wait_queue_match_fn matcher,
    void *match_arg, unsigned limit, wait_queue_rekey_fn rekey, void *rekey_arg)
{
    return wait_queue_requeue_matching(source, target, matcher, match_arg,
                                       limit, rekey, rekey_arg);
}

bool a20_futex_signal_unblocked(void *opaque_task)
{
    return signal_task_has_unblocked(opaque_task);
}

bool a20_futex_wake_is_interrupt(int reason)
{
    return proc_wake_reason_is_task_interrupt((proc_wake_reason_t)reason);
}

bool a20_futex_entry_task_dead(const wait_queue_entry_t *entry)
{
    return entry && entry->task &&
           (entry->task->state == PROC_UNUSED ||
            entry->task->state == PROC_ZOMBIE);
}

/* Native user atomics keep all word mutations in the same domain provider as
 * the wait-side recheck. No unprivileged compiler LDREX/LR.SC or direct access
 * to a privileged hardware gate is required. The numeric operations are the
 * internal LOAD/STORE/EXCHANGE/CAS contract, independent of any ABI header. */
int futex_atomic_user32(uint32_t *uaddr, unsigned operation, uint32_t expected,
                        uint32_t desired, uint32_t *observed,
                        uint32_t *exchanged) {
    if (!uaddr)
        return -EFAULT;
    if ((uintptr_t)uaddr & 3U || operation > 3 || !observed || !exchanged)
        return -EINVAL;
    task_t *task = proc_current();
    if (!task)
        return -ESRCH;
#ifndef CONFIG_NOMMU
    if (!task->mm)
        return -EFAULT;
    /* Write operations must resolve a writable/COW mapping before pinning. */
    if (operation) {
        int prepared = user_prepare_write(task, (uintptr_t)uaddr);
        if (prepared < 0)
            return prepared;
    }
#endif
    uint32_t probe;
    if (copy_from_user(&probe, uaddr, sizeof(probe)) < 0)
        return -EFAULT;
    uint64_t flags = futex_wait_mapping_lock(task);
#ifndef CONFIG_NOMMU
    /* Recheck permissions with the mapping pinned: another thread may have
     * replaced/protected it after user_prepare_write resolved COW. */
    mm_leaf_info_t leaf;
    uint64_t required = PTE_U | PTE_R | (operation ? PTE_W : 0);
    if (!mm_query_leaf(task->pgdir, (uintptr_t)uaddr, &leaf) ||
        (leaf.flags & required) != required) {
        futex_wait_mapping_unlock(task, flags);
        return -EFAULT;
    }
#endif
    uintptr_t pkey;
    volatile uint32_t *word =
        (volatile uint32_t *)futex_user_word_locked(task, (int *)uaddr, &pkey);
    if (!word) {
        futex_wait_mapping_unlock(task, flags);
        return -EFAULT;
    }
    *exchanged = 0;
    switch (operation) {
        case 0:
            *observed = __atomic_load_n(word, __ATOMIC_SEQ_CST);
            break;
        case 1:
            /* Exchanging also returns the previous value for STORE. */
        case 2:
            *observed = __atomic_exchange_n(word, desired, __ATOMIC_SEQ_CST);
            break;
        case 3:
            *exchanged =
                __atomic_compare_exchange_n(word, &expected, desired, 0,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *observed = expected;
            break;
    }
    futex_wait_mapping_unlock(task, flags);
    return 0;
}

/*
 * FUTEX_WAIT_RECHECK_PROTOCOL
 *
 * The copy_from_user() below may fault the page in.  The actual wait-side
 * linearization point is the non-faulting load taken with mm->lock and the
 * bucket lock both held: munmap cannot invalidate the translation, and a
 * matching FUTEX_WAKE cannot inspect the bucket until the waiter is linked.
 */
/* ticks == 0 waits indefinitely. */
static int futex_wait_ticks_pinned(int *uaddr, int expected, uint64_t ticks,
                                   uint32_t bitset, int private) {
    return a20_futex_wait_ticks_rust(uaddr, expected, ticks, bitset, private);
}

/* Dynamic flat words retain their allocation throughout wait admission,
 * sleep, wake and unlink. Unmap returns BUSY instead of reusing a live key. */
int futex_wait_ticks_mode(int *uaddr, int expected, uint64_t ticks,
                          uint32_t bitset, int private) {
#ifdef CONFIG_FLAT_USER_MEMORY
    task_t *task = proc_current();
    if (task && task->flat_user && uaddr && !((uintptr_t)uaddr & 3U) &&
        bitset) {
        a20_flat_region_t *lease =
            a20_flat_pin(task->flat_user, (uintptr_t)uaddr, sizeof(*uaddr));
        if (!lease)
            return -EFAULT;
        int result =
            futex_wait_ticks_pinned(uaddr, expected, ticks, bitset, private);
        a20_flat_unpin(task->flat_user, lease);
        return result;
    }
#endif
    return futex_wait_ticks_pinned(uaddr, expected, ticks, bitset, private);
}

int futex_wait_ticks(int *uaddr, int expected, uint64_t ticks, uint32_t bitset)
{
    return futex_wait_ticks_mode(uaddr, expected, ticks, bitset, 0);
}

int futex_wait(int *uaddr, int expected, void *timeout, uint32_t bitset,
                         int absolute_timeout, int realtime_timeout, int private)
{
    uint64_t ticks = 0;
    int tr = futex_timeout_ticks(timeout, absolute_timeout, realtime_timeout, &ticks);
    if (tr < 0) return tr;
    return futex_wait_ticks_mode(uaddr, expected, ticks, bitset, private);
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
        uint64_t room = UINT64_MAX - now;
        uint64_t fraction =
            (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
        if (sec > room / TICKS_PER_SEC) {
            ticks = room;
        } else {
            ticks = sec * TICKS_PER_SEC;
            ticks = fraction > room - ticks ? room : ticks + fraction;
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
    uintptr_t phys = private ? 0 : futex_phys_key(uaddr);
    uintptr_t pkey = phys;
    mm_struct_t *mm = cur ? cur->mm : NULL;
    unsigned home = a20_futex_bucket_index(vkey, phys);
    bool may_be_elsewhere = !private && phys == 0;
    futex_wake_arg_t arg = { mm, vkey, pkey, bitset };

    int woke = 0;
    while (woke < nr) {
        unsigned want = (unsigned)(nr - woke);
        if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        bool complete = false;
        unsigned got = futex_collect_below(home, may_be_elsewhere,
                                           a20_futex_wake_match, &arg,
                                           want, PROC_WAKE_EVENT, &wake_q,
                                           &complete);
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
    uintptr_t phys1 = private ? 0 : futex_phys_key(uaddr);
    uintptr_t pkey1 = private ? 0 : phys1;
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t phys2 = private ? 0 : futex_phys_key(uaddr2);
    uintptr_t pkey2 = private ? 0 : phys2;
    unsigned b1 = a20_futex_bucket_index(vkey1, phys1);
    unsigned b2 = a20_futex_bucket_index(vkey2, phys2);

    int done = 0;

    /* Wake phase on bucket1 (q1 lock). */
    futex_wake_arg_t arg1 = { mm, vkey1, pkey1, FUTEX_BITSET_MATCH_ANY };
    while (done < wake_nr) {
        unsigned want = (unsigned)(wake_nr - done);
        if (want > FUTEX_WAKE_BATCH) want = FUTEX_WAKE_BATCH;
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        unsigned got = futex_collect_below(b1, !private && phys1 == 0,
                                           a20_futex_wake_match, &arg1,
                                           want, PROC_WAKE_EVENT, &wake_q, NULL);
        if (got == 0)
            break;
        (void)proc_wake_q_flush(&wake_q);
        done += (int)got;
    }

    /*
     * Requeue phase: remaining bucket1 matches move to bucket2.
     */
    futex_wake_arg_t argr = { mm, vkey1, pkey1, FUTEX_BITSET_MATCH_ANY };
    futex_requeue_arg_t rarg = { vkey2, pkey2, mm };
    int moved = (int)a20_futex_requeue_transaction(
        b1, b2, &argr, &rarg, (unsigned)requeue_nr,
        !private && phys1 == 0);
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
    case FUTEX_OP_ADD:  *out = (int)((uint32_t)oldval + (uint32_t)oparg); return 0;
    case FUTEX_OP_OR:   *out = oldval | oparg; return 0;
    case FUTEX_OP_ANDN: *out = oldval & ~oparg; return 0;
    case FUTEX_OP_XOR:  *out = oldval ^ oparg; return 0;
    default: return -EINVAL;
    }
}

/* Preparation may fault/COW; mapping and the atomic store share mm->lock. */
static int futex_user_word_map(task_t *task, int *uaddr, volatile int **word,
                               uintptr_t *pkey_out);
/* Preparation may fault/COW; mapping and the atomic store share mm->lock.
 * See FUTEX_WRITABLE_WORD_WINDOW for the retry contract. */
static int futex_writable_word_locked(task_t *t, int *uaddr,
                                      volatile int **word_out,
                                      uintptr_t *pkey_out,
                                      uint64_t *mm_flags);

static unsigned futex_count_locked(wait_queue_t *q, futex_wake_arg_t *arg,
                                    unsigned limit)
{
    unsigned count = 0;
    for (wait_queue_entry_t *e = q->head; e && count < limit; e = e->next)
        if (a20_futex_wake_match(e, arg)) count++;
    return count;
}

static unsigned futex_collect_batches_locked(wait_queue_t *q,
                                    futex_wake_arg_t *arg, unsigned limit,
                                    proc_wake_q_t *batches, size_t nr_batches)
{
    unsigned total = 0;
    for (size_t i = 0; i < nr_batches && total < limit; i++) {
        unsigned got = wait_queue_collect_matching_locked(q, a20_futex_wake_match,
                          arg, limit - total, PROC_WAKE_EVENT, &batches[i], NULL);
        total += got;
        if (!got) break;
    }
    return total;
}

int futex_wake_op(int *uaddr, int wake_nr, int wake2_nr,
                         int *uaddr2, int encoded_op, int private)
{
    if (!uaddr || !uaddr2) return -EFAULT;
    if ((uintptr_t)uaddr > USER_VA_LIMIT - sizeof(int) ||
        (uintptr_t)uaddr2 > USER_VA_LIMIT - sizeof(int))
        return -EFAULT;
    if (((uintptr_t)uaddr | (uintptr_t)uaddr2) & (sizeof(int) - 1))
        return -EINVAL;
    if (wake_nr < 0 || wake2_nr < 0) return -EINVAL;

    uint32_t encoded = (uint32_t)encoded_op;
    int op = (int)((encoded >> 28) & 7);
    int cmp = (int)((encoded >> 24) & 0xf);
    int oparg = (int)(((encoded >> 12) & 0xfff) ^ 0x800) - 0x800;
    int cmparg = (int)((encoded & 0xfff) ^ 0x800) - 0x800;
    /* Match Linux's legacy masked-shift behavior, including bit 31. */
    if (encoded & ((uint32_t)FUTEX_OP_OPARG_SHIFT << 28))
        oparg = (int)(1U << ((unsigned)oparg & 31));
    if (op > FUTEX_OP_XOR) return -ENOSYS;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -ESRCH;
    mm_struct_t *mm = cur->mm;
    uintptr_t vkey1 = (uintptr_t)uaddr, vkey2 = (uintptr_t)uaddr2;
    for (;;) {
        int dummy;
        /* A shared key requires a resident translation. Private keys do not
         * dereference uaddr1, matching the wake side's virtual-key contract. */
        if (!private && copy_from_user(&dummy, uaddr, sizeof(dummy)) < 0)
            return -EFAULT;
        uint64_t mf = 0;
        volatile int *word = NULL;
        uintptr_t phys1 = 0, phys2 = 0;
        int r = futex_writable_word_locked(cur, uaddr2, &word, &phys2, &mf);
        if (r < 0) return r;
        if (!private && !futex_user_word_locked(cur, uaddr, &phys1)) {
            spin_unlock_irqrestore(&mm->lock, mf);
            continue;
        }
        if (private) phys2 = 0;
        unsigned b1 = a20_futex_bucket_index(vkey1, phys1);
        unsigned b2 = a20_futex_bucket_index(vkey2, phys2);
        wait_queue_t *q1 = &g_futex_buckets[b1], *q2 = &g_futex_buckets[b2];
        wait_queue_t *lo = b1 < b2 ? q1 : q2;
        wait_queue_t *hi = b1 < b2 ? q2 : q1;
        uint64_t lf = spin_lock_irqsave(&lo->lock), hf = 0;
        if (hi != lo) hf = spin_lock_irqsave(&hi->lock);
        futex_wake_arg_t arg1 = { mm, vkey1, phys1, FUTEX_BITSET_MATCH_ANY };
        futex_wake_arg_t arg2 = { mm, vkey2, phys2, FUTEX_BITSET_MATCH_ANY };
        unsigned count1 = cmp <= FUTEX_OP_CMP_GE ?
                          futex_count_locked(q1, &arg1, (unsigned)wake_nr) : 0;
        unsigned count2 = cmp <= FUTEX_OP_CMP_GE ?
                          futex_count_locked(q2, &arg2, (unsigned)wake2_nr) : 0;
        size_t n1 = ((size_t)count1 + PROC_WAKE_Q_CAPACITY - 1) / PROC_WAKE_Q_CAPACITY;
        size_t n2 = ((size_t)count2 + PROC_WAKE_Q_CAPACITY - 1) / PROC_WAKE_Q_CAPACITY;
        size_t n = n1 + n2;
        /* Reserve only for existing matches, not the caller's potentially
         * huge wake count. Allocate before modifying the word. All selected
         * references are detached while both buckets remain locked; flushing
         * batches afterwards must not select newly arrived waiters. */
        proc_wake_q_t *batches = n && n <= SIZE_MAX / sizeof(*batches) ?
                                  kcalloc_atomic(n, sizeof(*batches)) : NULL;
        int result = -ENOMEM;
        if (!n || batches) {
            int oldval = __atomic_load_n(word, __ATOMIC_SEQ_CST);
            int newval = oldval;
            do {
                (void)futex_wake_op_new_value(oldval, op, oparg, &newval);
            } while (!__atomic_compare_exchange_n(word, &oldval, newval, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
            /* Linux reports an unknown comparison after the atomic update,
             * but wakes neither queue. An unknown operation changes nothing. */
            result = -ENOSYS;
            if (cmp <= FUTEX_OP_CMP_GE) {
                result = (int)futex_collect_batches_locked(q1, &arg1, count1, batches, n1);
                if (futex_wake_op_cmp(oldval, cmp, cmparg) && count2)
                    result += (int)futex_collect_batches_locked(q2, &arg2,
                                                         count2, batches + n1, n2);
            }
        }
        if (hi != lo) spin_unlock_irqrestore(&hi->lock, hf);
        spin_unlock_irqrestore(&lo->lock, lf);
        spin_unlock_irqrestore(&mm->lock, mf);
        if (batches) {
            for (size_t i = 0; i < n; ++i)
                (void)proc_wake_q_flush(&batches[i]);
            kfree(batches);
        }
        return result;
    }
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
    uintptr_t pkey = 0;
    if (!task || !word)
        return -EFAULT;
#ifndef CONFIG_NOMMU
    /* prepare_write ran without mm->lock.  Fork may have write-protected
     * the leaf since then; never perform an atomic store into its COW frame.
     * The caller retries preparation with all locks dropped. */
    pte_t *pte = pt_lookup_leaf(task->pgdir, (vaddr_t)(uintptr_t)uaddr,
                                NULL, NULL, NULL);
    if (!pte || !arch_pte_is_leaf(*pte) ||
        (*pte & (PTE_V | PTE_U | PTE_W)) != (PTE_V | PTE_U | PTE_W))
        return -EAGAIN;
#endif
    volatile int *w = futex_user_word_locked(task, uaddr, &pkey);
    if (!w)
        return -EFAULT;
    *word = w;
    if (pkey_out)
        *pkey_out = pkey;
    return 0;
}

/*
 * FUTEX_WRITABLE_WORD_WINDOW
 *
 * A futex word must be both resolved writable and have its translation pinned
 * before the atomic store, and those two steps cannot share one lock
 * acquisition: user_prepare_write() breaks COW and must run without
 * mm->lock, while every read-modify-write of the word runs under it.  In
 * between, a concurrent fork can write-protect the leaf again, so the
 * translation must be re-validated under mm->lock and the whole preparation
 * retried -- never store into a frame that a fork just made COW-shared with a
 * child, or the store is invisible to (or worse, shared with) that child.
 *
 * Three call sites need exactly this protocol, so it lives here once instead
 * of being hand-rolled at each: futex_pi_acquire(), futex_pi_release() and
 * futex_wake_op().  They had drifted -- two retried with `continue`, one with
 * `goto retry` -- and a single owner of the decision also gives the COW-window
 * test one place to inject.
 *
 * On success returns 0 with mm->lock held and *mm_flags set; the caller
 * releases it with spin_unlock_irqrestore(&t->mm->lock, *mm_flags) once its
 * atomic store is done.  On failure returns a negative errno with no lock
 * held.  -EAGAIN never escapes: hitting the window is retried here, because
 * every caller had to do the same thing with it.
 */
#ifdef CONFIG_FUTEX_COW_WINDOW_TEST
/* Test injection, see kernel/include/uapi/a20/futex_cow_window.h: leave the
 * leaf in the write-protected state a concurrent fork would have produced
 * after user_prepare_write() resolved COW, once per task. */
static int futex_cow_window_skip_prepare(task_t *t, int *uaddr)
{
    /* One-shot per task, held outside task_t so the test build keeps the same
     * struct layout as production: only the single-threaded probe in
     * user/cmds/core/futex_cow_window_test.c ever addresses this page. */
    static int injected_pid;  /* pid_t is not a kernel type */
    uintptr_t va = (uintptr_t)uaddr & ~(uintptr_t)(PAGE_SIZE - 1);
    if (va != A20_FUTEX_COW_TEST_VA || injected_pid == t->pid)
        return 0;
    injected_pid = t->pid;
    kerr("[FUTEX-COW-WINDOW] pid=%d inject va=0x%lx\n", t->pid,
         (unsigned long)va);
    return 1;
}
#else
static inline int futex_cow_window_skip_prepare(task_t *t, int *uaddr)
{
    (void)t;
    (void)uaddr;
    return 0;
}
#endif

static int futex_writable_word_locked(task_t *t, int *uaddr,
                                      volatile int **word_out,
                                      uintptr_t *pkey_out,
                                      uint64_t *mm_flags)
{
    for (;;) {
        if (!futex_cow_window_skip_prepare(t, uaddr)) {
            int pr = user_prepare_write(t, (uint64_t)(uintptr_t)uaddr);
            if (pr < 0)
                return pr;
        }

        uint64_t flags = spin_lock_irqsave(&t->mm->lock);
        volatile int *word = NULL;
        uintptr_t pkey = 0;
        int mr = futex_user_word_map(t, uaddr, &word, &pkey);
        if (mr == 0) {
            *word_out = word;
            if (pkey_out)
                *pkey_out = pkey;
            *mm_flags = flags;
            return 0;
        }
        spin_unlock_irqrestore(&t->mm->lock, flags);
        if (mr != -EAGAIN)
            return mr;
    }
}

int futex_pi_acquire(int *uaddr, int try_only)
{
    if ((uintptr_t)uaddr & (sizeof(int) - 1))
        return -EINVAL;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint32_t tid = (uint32_t)t->pid;
    uintptr_t vkey = (uintptr_t)uaddr;

    for (;;) {
        uint64_t mm_flags = 0;
        volatile int *word = NULL;
        uintptr_t pkey = 0;
        int mr = futex_writable_word_locked(t, uaddr, &word, &pkey,
                                            &mm_flags);
        if (mr < 0)
            return mr;
        /* COW may change the physical key.  Derive the bucket only after the
         * writable translation has been validated under mm->lock. */
        wait_queue_t *q = &g_futex_buckets[
            a20_futex_bucket_index(vkey, pkey)];
        uint64_t q_flags = spin_lock_irqsave(&q->lock);
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
        /* Unlock may change the word before wait admission. Recheck ownership
         * on EAGAIN just as after a wake; it is not a failed blocking lock. */
        if (wr < 0 && wr != -ETIMEDOUT && wr != -EAGAIN)
            return wr;
    }
}

int futex_pi_release(int *uaddr)
{
    if ((uintptr_t)uaddr & (sizeof(int) - 1))
        return -EINVAL;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint32_t tid = (uint32_t)t->pid;
    uintptr_t vkey = (uintptr_t)uaddr;
    uint64_t mm_flags = 0;
    volatile int *word = NULL;
    uintptr_t pkey = 0;
    int mr = futex_writable_word_locked(t, uaddr, &word, &pkey, &mm_flags);
    if (mr < 0)
        return mr;
    wait_queue_t *q = &g_futex_buckets[
        a20_futex_bucket_index(vkey, pkey)];
    uint64_t q_flags = spin_lock_irqsave(&q->lock);
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

#endif
