#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "mm/mm.h"
#ifdef CONFIG_WAIT4_RUST
#include "proc/wait4_state.h"
#endif

#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8
#define WNOWAIT     0x1000000
#define __WNOTHREAD 0x20000000

/* WAIT4_COMMON_BEGIN */
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
/* WAIT4_COMMON_END */

#ifdef CONFIG_WAIT4_RUST

/*
 * WAIT4_RUST_ADAPTER:
 *
 * Under CONFIG_WAIT4_RUST the controller is kernel/proc/wait4_state.rs.  It
 * owns the wait4 decisions: child matching (pid selector, direct-child rule,
 * __WNOTHREAD scope, thread-group parent scope, pinned target identity),
 * reap/report classification (zombie / stopped / continued, group-dying
 * leader, still-on-CPU zombie, WNOWAIT), the stop/continue report decision and
 * wait-status encoding, and the scan -> reap / scan -> park transaction order.
 *
 * This adapter supplies only the mechanical primitives the controller orders:
 *
 *  - IRQ-save tasklist_lock acquire/release and the children / thread-group
 *    chain walks (group_start, member_next, child_first, child_next);
 *  - field sampling under tasklist_lock (child_view), with each child's own
 *    park_lock nested inside it via proc_task_state_get();
 *  - the park protocol: park_publish runs with tasklist_lock still held and
 *    publishes wait registration + the interruptible token under the waiter's
 *    park_lock, samples the unblocked-signal condition and self-wakes on it;
 *    the controller then orders scan_unlock, park_commit, park_finish,
 *    park_clear;
 *  - the reap transaction primitives: pin (proc_get, only when the controller
 *    asks), detach under tasklist_lock, then account/destroy/put after the
 *    scan lock is released (proc_destroy_task re-takes tasklist_lock itself);
 *  - the report decision: runs under the child's park_lock with the sampled
 *    stop/continue fields; the adapter applies the returned clear bits.
 *
 * No decision lives in these callbacks; each one is a single mechanical step.
 */
/* WAIT4_ADAPTER_BEGIN */
typedef struct wait4_adapter {
    task_t *self;    /* waiting task; proc_current() for the whole call */
    int *status;     /* the waiter's status slot, may be NULL */
    int options;
    proc_wait_token_t token;
} wait4_adapter_t;

static uint64_t wait4_adapter_scan_begin(void *context)
{
    (void)context;
    return spin_lock_irqsave(&tasklist_lock);
}

static void wait4_adapter_scan_unlock(void *context, uint64_t flags)
{
    (void)context;
    spin_unlock_irqrestore(&tasklist_lock, flags);
}

static void wait4_adapter_yield(void *context)
{
    (void)context;
    proc_yield();
}

static void *wait4_adapter_group_start(void *context, int whole_group)
{
    task_t *t = ((wait4_adapter_t *)context)->self;
    if (whole_group && t->tg_leader)
        return t->tg_leader;
    return t;
}

static void *wait4_adapter_member_next(void *context, void *node)
{
    (void)context;
    return ((task_t *)node)->tg_next;
}

static void *wait4_adapter_child_first(void *context, void *node)
{
    (void)context;
    return ((task_t *)node)->children;
}

static void *wait4_adapter_child_next(void *context, void *node)
{
    (void)context;
    return ((task_t *)node)->sibling_next;
}

/* Samples the fields the Rust matching and classification rules consume.
 * The chain walk and every sampled field are tasklist_lock-owned; only the
 * state sample nests the child's own park_lock (tasklist_lock -> park_lock).
 */
static int wait4_adapter_child_view(void *context, void *child_ptr,
                                    wait4_child_view_t *out)
{
    wait4_adapter_t *a = context;
    task_t *child = child_ptr;
    task_t *parent = child->parent;

    out->state = proc_task_state_get(child);
    out->pid = child->pid;
    out->pgid = child->pgid;
    out->parent_tgid = parent ? wait_task_tgid(parent) : -1;
    out->parent_pid = parent ? parent->pid : -1;
    out->waiter_tgid = wait_task_tgid(a->self);
    out->waiter_pid = a->self->pid;
    out->waiter_pgid = a->self->pgid;
    out->direct_child =
        (child->parent == a->self || child->ppid == a->self->pid) ? 1 : 0;
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->reserved[2] = 0;
    return 0;
}

static int wait4_adapter_group_dead(void *context, void *child)
{
    (void)context;
    return proc_tg_group_dead_locked(child);
}

static int wait4_adapter_is_current_any_cpu(void *context, void *child)
{
    (void)context;
    return proc_task_is_current_any_cpu(child);
}

/* Runs under the child's park_lock: samples the exit code and takes the pin
 * (proc_get, like the C reference) only when the controller asked for one.
 * The WNOWAIT report path never pins, so it cannot fail on a reference. */
static void *wait4_adapter_reap_pin(void *context, void *child_ptr,
                                    int32_t want_pin, int32_t *exit_code_out)
{
    (void)context;
    task_t *child = child_ptr;
    uint64_t cf = spin_lock_irqsave(&child->park_lock);
    int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
    if (exit_code_out)
        *exit_code_out = code;
    task_t *pin = want_pin ? proc_get(child) : child;
    spin_unlock_irqrestore(&child->park_lock, cf);
    return pin;
}

static void wait4_adapter_reap_detach(void *context, void *child)
{
    (void)context;
    proc_reap_detach_list_locked(child);
}

static void wait4_adapter_reap_release(void *context, void *child)
{
    wait4_adapter_t *a = context;
    wait_accumulate_child_time(a->self, child);
    proc_destroy_task(child);
    proc_put(child);
}

/* Runs under the child's park_lock while the Rust report decision is taken;
 * applies the returned clear bits (never requested under WNOWAIT). */
static int wait4_adapter_report_take(void *context, void *child_ptr,
                                     int32_t state)
{
    wait4_adapter_t *a = context;
    task_t *child = child_ptr;
    uint64_t cf = spin_lock_irqsave(&child->park_lock);
    wait4_report_view_t view = {
        .state = state,
        .options = a->options,
        .stop_report_pending = child->stop_report_pending,
        .continue_report_pending = child->continue_report_pending,
        .ptrace_stop_active = child->ptrace_stop_active,
        .ptrace_event = child->ptrace_event,
        .exit_code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE),
        .reserved = 0,
    };
    int action = a20_wait4_report_decide(&view, a->status);
    if (action & WAIT4_REPORT_CLEAR_STOP)
        child->stop_report_pending = 0;
    if (action & WAIT4_REPORT_CLEAR_CONT)
        child->continue_report_pending = 0;
    spin_unlock_irqrestore(&child->park_lock, cf);
    return action;
}

/* Runs with tasklist_lock still held: publishes wait registration and the
 * park token, samples the unblocked-signal condition and self-wakes on it.
 * Keeping tasklist_lock held through this publication closes the scan-to-park
 * lost-wakeup gap; the controller releases the scan lock after this returns.
 * The established lock order is tasklist_lock -> park_lock. */
static int wait4_adapter_park_publish(void *context)
{
    wait4_adapter_t *a = context;
    task_t *t = a->self;
    uint64_t plf = spin_lock_irqsave(&t->park_lock);
    t->waiting_for_child = 1;
    a->token = proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
    int sig = signal_task_has_unblocked(t);
    if (sig)
        (void)proc_try_wake_locked(t, a->token.seq, PROC_WAKE_SIGNAL);
    spin_unlock_irqrestore(&t->park_lock, plf);
    return sig;
}

static int wait4_adapter_park_commit(void *context)
{
    return (int)proc_park_commit(((wait4_adapter_t *)context)->token);
}

static void wait4_adapter_park_finish(void *context)
{
    proc_park_finish(((wait4_adapter_t *)context)->token);
}

static void wait4_adapter_park_clear(void *context)
{
    wait4_adapter_t *a = context;
    task_t *t = a->self;
    uint64_t pf2 = spin_lock_irqsave(&t->park_lock);
    /* A remote forced exit may already have removed this waiter while it
     * was parked. */
    if (t->waiting_for_child)
        t->waiting_for_child = 0;
    spin_unlock_irqrestore(&t->park_lock, pf2);
}

static int proc_wait4_internal(int pid, task_t *target_task, int *status,
                               int options)
{
    wait4_adapter_t adapter = {
        .self = proc_current(),
        .status = status,
        .options = options,
        .token = { .task = NULL, .seq = 0, .prepare_error = 0 },
    };
    const wait4_ops_t ops = {
        .context = &adapter,
        .scan_begin = wait4_adapter_scan_begin,
        .scan_unlock = wait4_adapter_scan_unlock,
        .yield_now = wait4_adapter_yield,
        .group_start = wait4_adapter_group_start,
        .member_next = wait4_adapter_member_next,
        .child_first = wait4_adapter_child_first,
        .child_next = wait4_adapter_child_next,
        .child_view = wait4_adapter_child_view,
        .group_dead = wait4_adapter_group_dead,
        .is_current_any_cpu = wait4_adapter_is_current_any_cpu,
        .reap_pin = wait4_adapter_reap_pin,
        .reap_detach = wait4_adapter_reap_detach,
        .reap_release = wait4_adapter_reap_release,
        .report_take = wait4_adapter_report_take,
        .park_publish = wait4_adapter_park_publish,
        .park_commit = wait4_adapter_park_commit,
        .park_finish = wait4_adapter_park_finish,
        .park_clear = wait4_adapter_park_clear,
    };
    return a20_wait4_run(&ops, pid, status, options, target_task);
}
/* WAIT4_ADAPTER_END */

#else /* !CONFIG_WAIT4_RUST */

/* WAIT4_C_ENGINE_BEGIN */
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
                                     int pid, int options,
                                     task_t *target_task)
{
    if (target_task && child != target_task)
        return 0;
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

static int proc_wait4_internal(int pid, task_t *target_task, int *status,
                               int options)
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
                    wait_child_matches_locked(child, t, pid, options,
                                              target_task)) {
                    found = 1;
                    if (cstate == PROC_ZOMBIE &&
                        !proc_tg_group_dead_locked(child)) {
                        /* zombie leader with live member threads: not
                         * reportable yet, the group is still dying */
                        child = next;
                        continue;
                    }
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

        /* Register before releasing tasklist_lock.  Child exit/reparent and
         * proc_wake_child_waiters() use tasklist_lock, so keeping it held
         * through this publication closes the scan-to-park lost-wakeup gap.
         * The established lock order is tasklist_lock -> park_lock. */
        uint64_t plf = spin_lock_irqsave(&t->park_lock);
        t->waiting_for_child = 1;
        proc_wait_token_t token =
            proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
        int sig = signal_task_has_unblocked(t);
        if (sig)
            (void)proc_try_wake_locked(t, token.seq, PROC_WAKE_SIGNAL);
        spin_unlock_irqrestore(&t->park_lock, plf);
        spin_unlock_irqrestore(&tasklist_lock, lock_flags);

        proc_wake_reason_t reason = proc_park_commit(token);
        proc_park_finish(token);

        uint64_t pf2 = spin_lock_irqsave(&t->park_lock);
        /* A remote forced exit may already have removed this waiter while it
         * was parked. */
        if (t->waiting_for_child)
            t->waiting_for_child = 0;
        spin_unlock_irqrestore(&t->park_lock, pf2);
        if (proc_wake_reason_is_task_interrupt(reason) || sig)
            return -ERESTARTSYS;
    }
}
/* WAIT4_C_ENGINE_END */

#endif /* CONFIG_WAIT4_RUST */

int proc_wait4(int pid, int *status, int options)
{
    return proc_wait4_internal(pid, NULL, status, options);
}

/* @target_task is pinned by the caller for the full wait. Matching by pointer
 * keeps P_PIDFD stable across PID reuse while retaining the ordinary child,
 * thread-group, wait-option, and reap rules. */
int proc_wait4_task(task_t *target_task, int *status, int options)
{
    if (!target_task)
        return -ESRCH;
    return proc_wait4_internal(-1, target_task, status, options);
}

int proc_wait(int *status)
{
    return proc_wait4(-1, status, 0);
}

#endif
