#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/klog.h"
#include "mm/mm.h"

#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8
#define WNOWAIT     0x1000000
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

static int wait_is_child_for_waiter_locked(task_t *child,
                                           task_t *waiting_task,
                                           int options)
{
    if (options & __WNOTHREAD)
        return wait_is_direct_child(child, waiting_task);

    if (wait_is_direct_child(child, waiting_task))
        return 1;

    int waiter_tgid = wait_task_tgid(waiting_task);
    if (wait_task_tgid(child->parent) == waiter_tgid)
        return 1;

    return 0;
}

static int wait_child_matches_locked(task_t *child, task_t *waiting_task,
                                     int pid, int options)
{
    if (!wait_is_child_for_waiter_locked(child, waiting_task, options))
        return 0;
    if (pid > 0 && child->pid != pid)
        return 0;
    if (pid == 0 && child->pgid != waiting_task->pgid)
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
        int reap_pending = 0;
        uint64_t lock_flags = spin_lock_irqsave(&proc_lock);
        for (task_t *child = proc_first_task_locked(); child;
             child = proc_next_task_locked(child)) {
            int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
            if (cstate == PROC_UNUSED) continue;
            if (!wait_child_matches_locked(child, t, pid, options)) continue;

            found = 1;
            if (cstate == PROC_ZOMBIE) {
                if (proc_task_is_current_any_cpu(child)) {
                    reap_pending = 1;
                    continue;
                }
                int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
                if (status) {
                    if (code >= 0)
                        *status = (code & 0xFF) << 8;
                    else
                        *status = (-code) & 0xFF;
                }
                int child_pid = child->pid;
                if (options & WNOWAIT) {
                    spin_unlock_irqrestore(&proc_lock, lock_flags);
                    return child_pid;
                }
                wait_accumulate_child_time(t, child);
                task_t *reap_child = proc_get(child);
                if (!reap_child) {
                    spin_unlock_irqrestore(&proc_lock, lock_flags);
                    return -ECHILD;
                }
                proc_reap_detach_locked(child);
                spin_unlock_irqrestore(&proc_lock, lock_flags);
                proc_destroy_task(reap_child);
                proc_put(reap_child);
                return child_pid;
            }
            if (cstate == PROC_STOPPED && child->stop_report_pending &&
                ((options & WUNTRACED) || child->ptrace_stop_active)) {
                int sig = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
                int child_pid = child->pid;
                if (status) {
                    if (child->ptrace_event)
                        *status = (child->ptrace_event << 16) |
                                  (SIGTRAP << 8) | 0x7F;
                    else
                        *status = (sig << 8) | 0x7F;
                }
                if (!(options & WNOWAIT))
                    child->stop_report_pending = 0;
                spin_unlock_irqrestore(&proc_lock, lock_flags);
                return child_pid;
            }
            if ((options & WCONTINUED) && child->continue_report_pending) {
                int child_pid = child->pid;
                if (status)
                    *status = 0xffff;
                if (!(options & WNOWAIT))
                    child->continue_report_pending = 0;
                spin_unlock_irqrestore(&proc_lock, lock_flags);
                return child_pid;
            }
        }

        if (reap_pending) {
            spin_unlock_irqrestore(&proc_lock, lock_flags);
            proc_yield();
            continue;
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
        proc_wait_token_t token =
            proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
        int sig = signal_task_has_unblocked(t);
        if (sig)
            (void)proc_try_wake_locked(t, token.seq, PROC_WAKE_SIGNAL);
        spin_unlock_irqrestore(&proc_lock, lock_flags);

        proc_wake_reason_t reason = proc_park_commit(token);
        proc_park_finish(token);

        uint64_t pf2 = spin_lock_irqsave(&proc_lock);
        t->waiting_for_child = 0;
        spin_unlock_irqrestore(&proc_lock, pf2);
        if (proc_wake_reason_is_task_interrupt(reason) || sig)
            return -ERESTARTSYS;
    }
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}
