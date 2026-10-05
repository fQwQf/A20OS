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

/* ->waiting_for_child and g_proc_waiting_child_waiter_count are always changed
 * together under the waiter's own park_lock; the counter is an advisory
 * fast-path hint for proc_wake_child_waiters() on other tasks. */

static void wait_accumulate_child_time(task_t *parent, task_t *child)
{
    if (!parent || !child)
        return;
    parent->child_utime += child->total_time;
    parent->child_stime += child->stime_ticks;
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

/*
 * WAIT4_CHILDREN_LIST_MODEL:
 * Every task is linked into its parent's children list and every
 * CLONE_THREAD task into its leader's thread-group chain (both under
 * tasklist_lock; debug attach/detach re-parents go through the same helpers).
 * wait4 therefore scans the waiting thread group's children lists --
 * O(children of the group) -- instead of the whole global task list.
 * Group coverage matches wait_is_child_for_waiter_locked(): a child whose
 * parent is any thread of the waiter's TGID is reapable by the caller.
 */
static task_t *wait_group_start_locked(task_t *t, int options)
{
    if (options & __WNOTHREAD)
        return t;
    return t->tg_leader ? t->tg_leader : t;
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
        /* tasklist_lock covers the thread-group and children chain walks plus
         * every reparent/unlink in them.  Each child's ->state, ->exit_code and
         * stop/continue report flags are park_lock-owned and are sampled under
         * that child's own park_lock nested inside it (tasklist_lock ->
         * park_lock); no two task park_locks are ever held at once (INV-P3). */
        uint64_t lock_flags = spin_lock_irqsave(&tasklist_lock);
        for (task_t *member = wait_group_start_locked(t, options); member;
             member = member->tg_next) {
            for (task_t *child = member->children; child; ) {
                task_t *next = child->sibling_next;
                int cstate = proc_task_state_get(child);
                if (cstate != PROC_UNUSED &&
                    wait_child_matches_locked(child, t, pid, options)) {
                    if (cstate == PROC_ZOMBIE &&
                        !proc_tg_group_dead_locked(child)) {
                        /* zombie leader with live member threads: not
                         * reportable yet, the group is still dying */
                        child = next;
                        continue;
                    }
                    found = 1;
                    if (cstate == PROC_ZOMBIE) {
                        if (proc_task_is_current_any_cpu(child)) {
                            reap_pending = 1;
                            child = next;
                            continue;
                        }
                        int child_pid = child->pid;
                        task_t *reap_child = NULL;
                        uint64_t cf = spin_lock_irqsave(&child->park_lock);
                        int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
                        if (status) {
                            if (code >= 0)
                                *status = (code & 0xFF) << 8;
                            else
                                *status = (-code) & 0xFF;
                        }
                        if (!(options & WNOWAIT)) {
                            /* proc_get() before the detach so the reference
                             * cannot be lost between the two. */
                            reap_child = proc_get(child);
                            spin_unlock_irqrestore(&child->park_lock, cf);
                            if (!reap_child) {
                                spin_unlock_irqrestore(&tasklist_lock, lock_flags);
                                return -ECHILD;
                            }
                            proc_reap_detach_list_locked(child);
                            spin_unlock_irqrestore(&tasklist_lock, lock_flags);
                            wait_accumulate_child_time(t, reap_child);
                            proc_destroy_task(reap_child);
                            proc_put(reap_child);
                            return child_pid;
                        }
                        spin_unlock_irqrestore(&child->park_lock, cf);
                        spin_unlock_irqrestore(&tasklist_lock, lock_flags);
                        return child_pid;
                    }
                    if (cstate == PROC_STOPPED || (options & WCONTINUED)) {
                        uint64_t cf = spin_lock_irqsave(&child->park_lock);
                        int take_stop = cstate == PROC_STOPPED &&
                            child->stop_report_pending &&
                            ((options & WUNTRACED) || child->ptrace_stop_active);
                        int take_cont = !take_stop && (options & WCONTINUED) &&
                            child->continue_report_pending;
                        int child_pid = child->pid;
                        if (take_stop) {
                            int sig = __atomic_load_n(&child->exit_code,
                                                      __ATOMIC_ACQUIRE);
                            if (status) {
                                if (child->ptrace_event)
                                    *status = (child->ptrace_event << 16) |
                                              (SIGTRAP << 8) | 0x7F;
                                else
                                    *status = (sig << 8) | 0x7F;
                            }
                            if (!(options & WNOWAIT))
                                child->stop_report_pending = 0;
                        } else if (take_cont && status) {
                            *status = 0xffff;
                        }
                        if (take_cont && !(options & WNOWAIT))
                            child->continue_report_pending = 0;
                        spin_unlock_irqrestore(&child->park_lock, cf);
                        if (take_stop || take_cont) {
                            spin_unlock_irqrestore(&tasklist_lock, lock_flags);
                            return child_pid;
                        }
                    }
                }
                child = next;
            }
        }

        if (reap_pending) {
            spin_unlock_irqrestore(&tasklist_lock, lock_flags);
            proc_yield();
            continue;
        }

        if (!found) {
            spin_unlock_irqrestore(&tasklist_lock, lock_flags);
            return -ECHILD;
        }

        if (options & WNOHANG) {
            spin_unlock_irqrestore(&tasklist_lock, lock_flags);
            return 0;
        }

        /* Drop the list lock before parking.  This branch is the only one in
         * proc_wait4() that reaches the bottom of the loop, and
         * proc_park_commit() switches to another task: holding tasklist_lock
         * across it parks the waiter as the lock's owner, so the next task
         * that needs the list (the first clone of a booting init, say) spins
         * on a lock nobody will ever release.  Every other exit above already
         * released it at the same place.  The scan result stays valid: the
         * waiter registration below is guarded by t->park_lock alone, and the
         * loop re-scans the whole group after every wake. */
        spin_unlock_irqrestore(&tasklist_lock, lock_flags);

        /* The waiter registration and the park preparation are one park_lock
         * critical section: ->waiting_for_child and the aggregate counter stay
         * paired with the park state machine (INV-P1). */
        uint64_t plf = spin_lock_irqsave(&t->park_lock);
        t->waiting_for_child = 1;
        g_proc_waiting_child_waiter_count++;
        proc_wait_token_t token =
            proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
        int sig = signal_task_has_unblocked(t);
        if (sig)
            (void)proc_try_wake_locked(t, token.seq, PROC_WAKE_SIGNAL);
        spin_unlock_irqrestore(&t->park_lock, plf);

        proc_wake_reason_t reason = proc_park_commit(token);
        proc_park_finish(token);

        uint64_t pf2 = spin_lock_irqsave(&t->park_lock);
        /* A remote forced exit may already have removed this waiter while it
         * was parked.  Update the flag and the aggregate as one guarded
         * transition so the fast path can never under-count live waiters. */
        if (t->waiting_for_child) {
            t->waiting_for_child = 0;
            if (g_proc_waiting_child_waiter_count)
                g_proc_waiting_child_waiter_count--;
        }
        spin_unlock_irqrestore(&t->park_lock, pf2);
        if (proc_wake_reason_is_task_interrupt(reason) || sig)
            return -ERESTARTSYS;
    }
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}
