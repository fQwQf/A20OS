#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "proc/wait4_state.h"

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


/*
 * WAIT4_RUST_ADAPTER:
 *
 * The controller is kernel/proc/wait4_state.rs.  It
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
