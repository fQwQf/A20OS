#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/klog.h"

#define WNOHANG 1

static void wait_accumulate_child_time(task_t *parent, task_t *child)
{
    if (!parent || !child)
        return;
    parent->child_utime += child->total_time;
}

int proc_wait4(int pid, int *status, int options)
{
    task_t *t = proc_current();

retry:;
    int found = 0;
    uint64_t lock_flags = spin_lock_irqsave(&proc_lock);
    for (task_t *child = proc_first_task_locked(); child;
         child = proc_next_task_locked(child)) {
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        found = 1;
        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0xFF;
            }
            int child_pid = child->pid;
            wait_accumulate_child_time(t, child);
            spin_unlock_irqrestore(&proc_lock, lock_flags);
            proc_free_pid(child_pid);
            return child_pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, lock_flags);

    if (!found)
        return -ECHILD;

    if (options & WNOHANG)
        return 0;

    if (signal_task_has_unblocked(t))
        return -EINTR;

    /*
     * Avoid the classic lost-wakeup race: mark the parent blocked while
     * holding proc_lock, then rescan zombies before dropping the lock.
     */
    lock_flags = spin_lock_irqsave(&proc_lock);
    t->state = PROC_BLOCKED;
    for (task_t *child = proc_first_task_locked(); child;
         child = proc_next_task_locked(child)) {
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            t->state = PROC_RUNNING;
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0xFF;
            }
            int child_pid = child->pid;
            wait_accumulate_child_time(t, child);
            spin_unlock_irqrestore(&proc_lock, lock_flags);
            proc_free_pid(child_pid);
            return child_pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, lock_flags);

    if (t->state == PROC_BLOCKED)
        sched();
    t->state = PROC_RUNNING;
    if (signal_task_has_unblocked(t))
        return -EINTR;
    goto retry;
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}
