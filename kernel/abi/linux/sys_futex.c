#include "syscall_impl.h"
#include "sys/futex.h"
#include "abi/linux/futex.h"
#include "core/lock.h"

#define FUTEX_WAITERS_MAX 256

typedef struct futex_waiter {
    int active;
    int woken;
    uintptr_t vkey;
    uintptr_t pkey;
    mm_struct_t *mm;
    uint32_t bitset;
    task_t *task;
} futex_waiter_t;

static spinlock_t g_futex_lock = SPINLOCK_INIT;
static futex_waiter_t g_futex_waiters[FUTEX_WAITERS_MAX];

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
                         (uint64_t)ts[1] * TICKS_PER_SEC / 1000000000ULL;
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
    uint64_t ticks = sec * TICKS_PER_SEC + nsec * TICKS_PER_SEC / 1000000000ULL;
    *ticks_out = ticks ? ticks : 1;
    return 0;
}

static void futex_waiter_clear_slot(futex_waiter_t *w)
{
    w->active = 0;
    w->woken = 0;
    w->task = NULL;
    w->vkey = 0;
    w->pkey = 0;
    w->mm = NULL;
    w->bitset = 0;
}

static void futex_waiter_clear_task(task_t *task)
{
    if (!task) return;
    for (int i = 0; i < FUTEX_WAITERS_MAX; i++) {
        if (g_futex_waiters[i].active && g_futex_waiters[i].task == task)
            futex_waiter_clear_slot(&g_futex_waiters[i]);
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
    if (w->woken)
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
                              uint32_t bitset, task_t *task)
{
    futex_waiter_clear_task(task);
    for (int i = 0; i < FUTEX_WAITERS_MAX; i++) {
        if (!g_futex_waiters[i].active) {
            g_futex_waiters[i].active = 1;
            g_futex_waiters[i].woken = 0;
            g_futex_waiters[i].vkey = vkey;
            g_futex_waiters[i].pkey = pkey;
            g_futex_waiters[i].mm = mm;
            g_futex_waiters[i].bitset = bitset;
            g_futex_waiters[i].task = task;
            return i;
        }
    }
    return -ENOMEM;
}

static int futex_wait_on(int *uaddr, int expected, void *timeout, uint32_t bitset,
                         int absolute_timeout, int realtime_timeout)
{
    if (!uaddr) return -EFAULT;
    if (bitset == 0) return -EINVAL;

    task_t *t = proc_current();
    if (!t) return -ESRCH;

    int uval;
    if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) return -EFAULT;

    uint64_t ticks = 0;
    int tr = futex_timeout_ticks(timeout, absolute_timeout, realtime_timeout, &ticks);
    if (tr < 0) return tr;
    uint64_t until = ticks ? timer_get_ticks() + ticks : 0;

    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    uintptr_t vkey = (uintptr_t)uaddr;
    uintptr_t pkey = futex_phys_key(uaddr);
    if (copy_from_user(&uval, uaddr, sizeof(uval)) < 0) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return -EFAULT;
    }
    if (uval != expected) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return -EAGAIN;
    }
    if (signal_task_has_unblocked(t)) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return -ERESTARTSYS;
    }
    int slot = futex_waiter_alloc(vkey, pkey, t->mm, bitset, t);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_futex_lock, flags);
        return slot;
    }
    proc_set_wake_time(t, until);
    t->state = PROC_BLOCKED;
    spin_unlock_irqrestore(&g_futex_lock, flags);

    sched();

    flags = spin_lock_irqsave(&g_futex_lock);
    int was_woken = 0;
    int still_waiting = 0;
    if (slot >= 0 && slot < FUTEX_WAITERS_MAX) {
        futex_waiter_t *w = &g_futex_waiters[slot];
        was_woken = w->woken;
        if (w->active && w->task == t) {
            still_waiting = 1;
            futex_waiter_clear_slot(w);
        } else {
            futex_waiter_clear_slot(w);
        }
    }
    proc_set_wake_time(t, 0);
    spin_unlock_irqrestore(&g_futex_lock, flags);

    if (was_woken)
        return 0;
    if (signal_task_has_unblocked(t))
        return -ERESTARTSYS;
    if (!was_woken && still_waiting && until && timer_get_ticks() >= until)
        return -ETIMEDOUT;
    return 0;
}

static int futex_wake_on(int *uaddr, int nr, uint32_t bitset)
{
    if (!uaddr) return -EFAULT;
    if (bitset == 0) return -EINVAL;
    if (nr < 0) return -EINVAL;

    int woke = 0;
    uintptr_t vkey = (uintptr_t)uaddr;
    uintptr_t pkey = futex_phys_key(uaddr);
    task_t *cur = proc_current();
    mm_struct_t *mm = cur ? cur->mm : NULL;
    task_t *wake_list[FUTEX_WAITERS_MAX];
    int wake_count = 0;
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    for (int i = 0; i < FUTEX_WAITERS_MAX && woke < nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey, pkey) || !(w->bitset & bitset))
            continue;
        task_t *task = w->task;
        w->woken = 1;
        if (task) {
            wake_list[wake_count++] = task;
            woke++;
        }
    }
    spin_unlock_irqrestore(&g_futex_lock, flags);
    for (int i = 0; i < wake_count; i++) {
        task_t *task = wake_list[i];
        proc_set_wake_time(task, 0);
        proc_make_ready(task);
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
    task_t *wake_list[FUTEX_WAITERS_MAX];
    int wake_count = 0;
    uintptr_t vkey1 = (uintptr_t)uaddr;
    uintptr_t pkey1 = futex_phys_key(uaddr);
    uintptr_t vkey2 = (uintptr_t)uaddr2;
    uintptr_t pkey2 = futex_phys_key(uaddr2);
    task_t *cur = proc_current();
    mm_struct_t *mm = cur ? cur->mm : NULL;
    uint64_t flags = spin_lock_irqsave(&g_futex_lock);
    for (int i = 0; i < FUTEX_WAITERS_MAX && done < wake_nr; i++) {
        futex_waiter_t *w = &g_futex_waiters[i];
        if (!futex_waiter_matches(w, mm, vkey1, pkey1)) continue;
        task_t *task = w->task;
        w->woken = 1;
        if (task) {
            wake_list[wake_count++] = task;
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
        task_t *task = wake_list[i];
        proc_set_wake_time(task, 0);
        proc_make_ready(task);
    }
    return done + moved;
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
