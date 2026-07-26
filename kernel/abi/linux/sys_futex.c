#include "syscall_impl.h"
#include "sys/futex.h"
#include "abi/linux/futex.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "core/lock.h"

#define FUTEX_WAITERS_MAX 1024

typedef struct futex_waiter {
    int active;
    uintptr_t vkey;
    uintptr_t pkey;
    mm_struct_t *mm;
    uint32_t bitset;
    task_t *task;
    uint64_t wait_seq;
} futex_waiter_t;

typedef struct futex_wake_token {
    task_t *task;
    uint64_t wait_seq;
} futex_wake_token_t;

static spinlock_t g_futex_lock = SPINLOCK_INIT;
static futex_waiter_t g_futex_waiters[FUTEX_WAITERS_MAX];
static uint64_t g_futex_wake_generation;

static int futex_timeout_ticks(void *timeout, int absolute, int realtime,
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

/* Remove a waiter and transfer its task reference to the caller. */
static task_t *futex_waiter_take_slot(futex_waiter_t *w)
{
    task_t *task = w->active ? w->task : NULL;
    if (w->active)
        proc_lifetime_note_wait_remove();
    w->active = 0;
    w->task = NULL;
    w->wait_seq = 0;
    w->vkey = 0;
    w->pkey = 0;
    w->mm = NULL;
    w->bitset = 0;
    return task;
}

static void futex_waiter_clear_task(task_t *task)
{
    if (!task) return;
    for (int i = 0; i < FUTEX_WAITERS_MAX; i++) {
        if (g_futex_waiters[i].active && g_futex_waiters[i].task == task) {
            task_t *old = futex_waiter_take_slot(&g_futex_waiters[i]);
            proc_put(old);
        }
    }
}

static uintptr_t futex_phys_key(int *uaddr)
{
    task_t *t = proc_current();
    if (!t || !t->pgdir || !uaddr) return (uintptr_t)uaddr;
    paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)uaddr);
    return pa ? (uintptr_t)pa : 0;
}

static int futex_waiter_matches(const futex_waiter_t *w, mm_struct_t *mm,
                                uintptr_t vkey, uintptr_t pkey)
{
    if (!w->active)
        return 0;
    if (w->task && (w->task->state == PROC_UNUSED || w->task->state == PROC_ZOMBIE))
        return 0;
    if (w->mm == mm && w->vkey == vkey)
        return 1;
    if (pkey && w->pkey == pkey)
        return 1;
    return 0;
}

static int futex_waiter_alloc(uintptr_t vkey, uintptr_t pkey, mm_struct_t *mm,
                              uint32_t bitset, proc_wait_token_t token)
{
    futex_waiter_clear_task(token.task);
    for (int i = 0; i < FUTEX_WAITERS_MAX; i++) {
        if (!g_futex_waiters[i].active) {
            task_t *task = proc_get(token.task);
            if (!task)
                return -ESRCH;
            g_futex_waiters[i].active = 1;
            g_futex_waiters[i].vkey = vkey;
            g_futex_waiters[i].pkey = pkey;
            g_futex_waiters[i].mm = mm;
            g_futex_waiters[i].bitset = bitset;
            g_futex_waiters[i].task = task;
            g_futex_waiters[i].wait_seq = token.seq;
            proc_lifetime_note_wait_add();
            return i;
        }
    }
    return -ENOMEM;
}

/* g_futex_lock protects waiter publication/removal; proc_try_wake() is always
 * called after dropping it, using wait_seq to reject stale wakeups. */
static int futex_wait_on(int *uaddr, int expected, void *timeout, uint32_t bitset,
                         int absolute_timeout, int realtime_timeout)
{
    if (!uaddr) return -EFAULT;
    if (bitset == 0) return -EINVAL;

    task_t *t = proc_current();
    if (!t) return -ESRCH;

    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    uint64_t wait_generation = g_futex_wake_generation;
    spin_unlock_irqrestore(&g_futex_lock, flags);

    int uval;
    if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) return -EFAULT;
    if (uval != expected) return -EAGAIN;

    uint64_t ticks = 0;
    int tr = futex_timeout_ticks(timeout, absolute_timeout, realtime_timeout, &ticks);
    if (tr < 0) return tr;
    uint64_t until = ticks ? timer_get_ticks() + ticks : 0;
    uintptr_t vkey = (uintptr_t)uaddr;
    uintptr_t pkey = futex_phys_key(uaddr);

    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, until);
    if (!token.task)
        return -EAGAIN;

    flags = spin_lock_irqsave(&g_futex_lock);
    if (wait_generation != g_futex_wake_generation) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return 0;
    }
    if (signal_task_has_unblocked(t)) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return -ERESTARTSYS;
    }
    int slot = futex_waiter_alloc(vkey, pkey, t->mm, bitset, token);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return slot;
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);

    proc_wake_reason_t reason = proc_park_commit(token);
    proc_park_finish(token);

    flags = spin_lock_irqsave(&g_futex_lock);
    task_t *waiter_ref = NULL;
    if (slot >= 0 && slot < FUTEX_WAITERS_MAX) {
        futex_waiter_t *w = &g_futex_waiters[slot];
        if (w->active && w->task == t && w->wait_seq == token.seq)
            waiter_ref = futex_waiter_take_slot(w);
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
    proc_put(waiter_ref);

    if (reason == PROC_WAKE_SIGNAL || signal_task_has_unblocked(t))
        return -ERESTARTSYS;
    if (reason == PROC_WAKE_TIMEOUT)
        return -ETIMEDOUT;
    return 0;
}

static int futex_wake_on(int *uaddr, int nr, uint32_t bitset)
{
    task_t *cur = proc_current();
    if (!uaddr) return -EFAULT;
    if (bitset == 0) return -EINVAL;
    if (nr < 0) return -EINVAL;

    uintptr_t vkey = (uintptr_t)uaddr;
    uintptr_t pkey = futex_phys_key(uaddr);
    mm_struct_t *mm = cur ? cur->mm : NULL;
    futex_wake_token_t wake_list[FUTEX_WAITERS_MAX];
    int wake_count = 0;
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    if (nr > 0)
        g_futex_wake_generation++;
    for (int i = 0; i < FUTEX_WAITERS_MAX && wake_count < nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey, pkey) || !(w->bitset & bitset))
            continue;
        uint64_t wait_seq = w->wait_seq;
        task_t *task = futex_waiter_take_slot(w);
        if (task) {
            wake_list[wake_count].task = task;
            wake_list[wake_count].wait_seq = wait_seq;
            wake_count++;
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
    int woke = 0;
    for (int i = 0; i < wake_count; i++) {
        woke += proc_try_wake(wake_list[i].task, wake_list[i].wait_seq,
                              PROC_WAKE_EVENT) ? 1 : 0;
        proc_put(wake_list[i].task);
    }
    return woke;
}

int futex_wake_user(int *uaddr, int nr)
{
    return futex_wake_on(uaddr, nr, FUTEX_BITSET_MATCH_ANY);
}

static int futex_requeue(int *uaddr, int wake_nr, int requeue_nr, int *uaddr2,
                         int check_cmp, int cmpval)
{
    if (!uaddr || !uaddr2) return -EFAULT;
    if (wake_nr < 0 || requeue_nr < 0) return -EINVAL;
    if (check_cmp) {
        int uval;
        if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) return -EFAULT;
        if (uval != cmpval) return -EAGAIN;
    }

    int done = 0;
    int moved = 0;
    futex_wake_token_t wake_list[FUTEX_WAITERS_MAX];
    int wake_count = 0;
    uintptr_t vkey1 = (uintptr_t)uaddr;
    uintptr_t pkey1 = futex_phys_key(uaddr);
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t pkey2 = futex_phys_key(uaddr2);
    task_t *cur = proc_current();
    mm_struct_t *mm = cur ? cur->mm : NULL;
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    if (wake_nr > 0 || requeue_nr > 0)
        g_futex_wake_generation++;
    for (int i = 0; i < FUTEX_WAITERS_MAX && done < wake_nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey1, pkey1)) continue;
        uint64_t wait_seq = w->wait_seq;
        task_t *task = futex_waiter_take_slot(w);
        if (task) {
            wake_list[wake_count].task = task;
            wake_list[wake_count].wait_seq = wait_seq;
            wake_count++;
            done++;
        }
    }
    for (int i = 0; i < FUTEX_WAITERS_MAX && moved < requeue_nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey1, pkey1)) continue;
        w->vkey = vkey2;
        w->pkey = pkey2;
        w->mm = mm;
        moved++;
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
    for (int i = 0; i < wake_count; i++) {
        (void)proc_try_wake(wake_list[i].task, wake_list[i].wait_seq,
                            PROC_WAKE_EVENT);
        proc_put(wake_list[i].task);
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

static int futex_wake_op(int *uaddr, int wake_nr, int wake2_nr,
                         int *uaddr2, int encoded_op)
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
    uintptr_t pkey1 = futex_phys_key(uaddr);
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t pkey2 = futex_phys_key(uaddr2);
    futex_wake_token_t wake_list[FUTEX_WAITERS_MAX];
    int wake_count = 0;
    int woke = 0;

    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    if (wake_nr > 0 || wake2_nr > 0)
        g_futex_wake_generation++;

    for (int i = 0; i < FUTEX_WAITERS_MAX && woke < wake_nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey1, pkey1))
            continue;
        uint64_t wait_seq = w->wait_seq;
        task_t *task = futex_waiter_take_slot(w);
        if (task) {
            wake_list[wake_count].task = task;
            wake_list[wake_count].wait_seq = wait_seq;
            wake_count++;
            woke++;
        }
    }

    if (futex_wake_op_cmp(oldval, cmp, cmparg)) {
        int woke2 = 0;
        for (int i = 0; i < FUTEX_WAITERS_MAX && woke2 < wake2_nr; i++) {
            futex_waiter_t *w = &g_futex_waiters[i];
            if (!futex_waiter_matches(w, mm, vkey2, pkey2))
                continue;
            uint64_t wait_seq = w->wait_seq;
            task_t *task = futex_waiter_take_slot(w);
            if (task) {
                wake_list[wake_count].task = task;
                wake_list[wake_count].wait_seq = wait_seq;
                wake_count++;
                woke++;
                woke2++;
            }
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);

    for (int i = 0; i < wake_count; i++) {
        (void)proc_try_wake(wake_list[i].task, wake_list[i].wait_seq,
                            PROC_WAKE_EVENT);
        proc_put(wake_list[i].task);
    }
    return woke;
}

int64_t sys_futex(int *uaddr, int op, int val, void *timeout, int *uaddr2, int val3)
{
    int opc = op & FUTEX_CMD_MASK;
    switch (opc) {
    case FUTEX_WAIT:
        return futex_wait_on(uaddr, val, timeout, FUTEX_BITSET_MATCH_ANY, 0, 0);
    case FUTEX_WAIT_BITSET:
        return futex_wait_on(uaddr, val, timeout, (uint32_t)val3, 1,
                             (op & FUTEX_CLOCK_REALTIME) != 0);
    case FUTEX_WAKE:
        return futex_wake_on(uaddr, val, FUTEX_BITSET_MATCH_ANY);
    case FUTEX_WAKE_BITSET:
        return futex_wake_on(uaddr, val, (uint32_t)val3);
    case FUTEX_REQUEUE:
        return futex_requeue(uaddr, val, (int)(intptr_t)timeout, uaddr2, 0, 0);
    case FUTEX_CMP_REQUEUE:
        return futex_requeue(uaddr, val, (int)(intptr_t)timeout, uaddr2, 1, val3);
    case FUTEX_WAKE_OP:
        return futex_wake_op(uaddr, val, (int)(intptr_t)timeout, uaddr2, val3);
    default:
        return -ENOSYS;
    }
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
