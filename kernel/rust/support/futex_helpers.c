#include "proc/proc.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/timekeeping.h"
#include "core/timer.h"

int a20_futex_task_state(task_t *task)
{
    return task ? (int)task->state : -1;
}

mm_struct_t *a20_futex_task_mm(task_t *task)
{
    return task ? task->mm : NULL;
}

int a20_futex_task_pid(task_t *task)
{
    return task ? task->pid : -1;
}

uintptr_t a20_futex_task_robust_list_head(task_t *task)
{
    return task ? task->robust_list_head : 0;
}

void a20_futex_task_clear_robust_list_head(task_t *task)
{
    if (task)
        task->robust_list_head = 0;
}

uintptr_t a20_futex_phys_key(int *uaddr)
{
    task_t *t = proc_current();
    if (!t || !t->pgdir || !uaddr)
        return (uintptr_t)uaddr;
    paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)uaddr);
    return pa ? (uintptr_t)pa : 0;
}

int a20_futex_timeout_ticks(void *timeout, int absolute, int realtime,
                            uint64_t *ticks_out)
{
    *ticks_out = 0;
    if (!timeout)
        return 0;

    int64_t ts[2];
    if (copy_from_user(ts, timeout, sizeof(ts)) < 0)
        return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL)
        return -EINVAL;

    if (!absolute) {
        uint64_t ticks = (uint64_t)ts[0] * TICKS_PER_SEC +
                         ((uint64_t)ts[1] * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
        *ticks_out = ticks ? ticks : 1;
        return 0;
    }

    uint64_t now_ts[2];
    if (realtime)
        timekeeping_get_realtime(now_ts);
    else
        timekeeping_get_monotonic(now_ts);
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

void a20_futex_get_monotonic(uint64_t *now)
{
    timekeeping_get_monotonic(now);
}

void a20_futex_get_realtime(uint64_t *now)
{
    timekeeping_get_realtime(now);
}

uint64_t a20_futex_get_ticks(void)
{
    return timer_get_ticks();
}

void a20_futex_set_blocked(task_t *task)
{
    if (task)
        task->state = PROC_BLOCKED;
}
