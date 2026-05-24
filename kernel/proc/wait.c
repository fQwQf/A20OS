#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/klog.h"
#include "mm/mm.h"

#define WNOHANG     1
#define WUNTRACED   2
#define __WNOTHREAD 0x20000000

static void wait_accumulate_child_time(task_t *parent, task_t *child)
{
    if (!parent || !child)
        return;
    parent->child_utime += child->total_time;
}

static int wait_task_tgid(task_t *t)
{
    return t ? (t->tgid > 0 ? t->tgid : t->pid) : -1;
}

static int wait_is_direct_child(task_t *child, task_t *parent)
{
    return child && parent && (child->parent == parent || child->ppid == parent->pid);
}

static int wait_is_child_for_waiter_locked(task_t *child, task_t *waiter, int options)
{
    if (options & __WNOTHREAD)
        return wait_is_direct_child(child, waiter);

    if (wait_is_direct_child(child, waiter))
        return 1;

    int waiter_tgid = wait_task_tgid(waiter);
    if (wait_task_tgid(child->parent) == waiter_tgid)
        return 1;

    return 0;
}

static int wait_child_matches_locked(task_t *child, task_t *waiter,
                                     int pid, int options)
{
    if (!wait_is_child_for_waiter_locked(child, waiter, options))
        return 0;
    if (pid > 0 && child->pid != pid)
        return 0;
    if (pid == 0 && child->pgid != waiter->pgid)
        return 0;
    if (pid < -1 && child->pgid != (-pid))
        return 0;
    return 1;
}

int proc_wait4(int pid, int *status, int options)
{
    task_t *t = proc_current();

    for (;;) {
        int found = 0;
        uint64_t lock_flags = spin_lock_irqsave(&proc_lock);
        for (task_t *child = proc_first_task_locked(); child;
             child = proc_next_task_locked(child)) {
            int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
            if (cstate == PROC_UNUSED) continue;
            if (!wait_child_matches_locked(child, t, pid, options)) continue;

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
                child->state = PROC_UNUSED;
                proc_unlink_task_locked(child);
                spin_unlock_irqrestore(&proc_lock, lock_flags);
                proc_pid_unregister(child);
                proc_task_release_resources(child);
                kfree(child);
                return child_pid;
            }
            if ((options & WUNTRACED) && cstate == PROC_STOPPED) {
                int sig = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
                if (status) {
                    *status = (sig << 8) | 0x7F;
                }
                spin_unlock_irqrestore(&proc_lock, lock_flags);
                return child->pid;
            }
        }

        if (!found) {
            spin_unlock_irqrestore(&proc_lock, lock_flags);
            return -ECHILD;
        }

        if (options & WNOHANG) {
            spin_unlock_irqrestore(&proc_lock, lock_flags);
            return 0;
        }

        t->waiting_for_child = 1;
        t->state = PROC_BLOCKED;
        int sig = signal_task_has_unblocked(t);
        spin_unlock_irqrestore(&proc_lock, lock_flags);

        if (sig) {
            uint64_t pf2 = spin_lock_irqsave(&proc_lock);
            t->waiting_for_child = 0;
            t->state = PROC_RUNNING;
            spin_unlock_irqrestore(&proc_lock, pf2);
            return -ERESTARTSYS;
        }

        if (t->state == PROC_BLOCKED)
            sched();
        uint64_t pf2 = spin_lock_irqsave(&proc_lock);
        t->waiting_for_child = 0;
        t->state = PROC_RUNNING;
        spin_unlock_irqrestore(&proc_lock, pf2);
    }
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}
