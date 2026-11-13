#include "cluster/ssi.h"
#include "proc/proc.h"
#include "proc/task_object.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "proc/debug.h"
#include "proc/acct.h"
#include "ext/kep.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "drivers/core/udriver.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
extern void a20_registry_task_exit(int pid);
#endif
/* Slot ownership in the Linux ABI hypervisor bridge is keyed on the pid, and
 * pids are recycled, so the keys have to go before the pid does. */
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
#include "hyp/hyp_bridge.h"
#endif
extern void udisk_task_exit(int pid);
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/swap.h"
#include "core/panic.h"
#include "core/string.h"
#include "sys/futex.h"
#include "ipc/ipc.h"
#include "sys/usercopy.h"
#include "cg/cgroup.h"
#include "proc/task_exit.h"
#include "proc/exit_reparent.h"

static int proc_ignores_sigchld(task_t *parent)
{
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    /* Native tasks register signal handlers in the libc (mlibc checkpoint
     * model), not in the kernel signal state, so the kernel's SIGCHLD
     * auto-reap heuristic cannot see them.  Always deliver SIGCHLD to a
     * native parent; the child becomes a zombie until the native parent
     * reaps it (task_wait). */
    if (parent && parent->abi_mode == 1)
        return 0;
#endif
    return signal_task_sigchld_auto_reap(parent);
}

static int proc_task_tgid(task_t *t)
{
    return t ? (t->tgid > 0 ? t->tgid : t->pid) : -1;
}

/* A zombie thread-group leader must not be reaped while any of its live
 * member threads remain.  The leader anchors the tg_next chain -- its own
 * proc_tg_unlink_locked() is a no-op because it has no tg_prev_ptr -- so
 * freeing it early leaves members traversing freed memory on their next
 * group walk (proc_find_live_thread_reaper_locked / wait_group_start).
 *
 * Chain membership is tasklist_lock; each member's ->state is park_lock-owned,
 * so every member is sampled under its own park_lock, one at a time.  Only
 * ->tgid/->pid are read from the leader, and both are fixed at clone time.
 * Holding no task lock across the whole walk is deliberate: it is what keeps
 * this callable from a site that already holds another task's park_lock
 * without creating an INV-P3 ordering constraint. */
int proc_tg_group_dead_locked(task_t *t)
{
    if (!t || t->tg_leader != t)
        return 1;               /* members never anchor the chain */
    int tgid = proc_task_tgid(t);
    for (task_t *m = t->tg_next; m; m = m->tg_next) {
        if (m == t)
            continue;
        int mstate = proc_task_state_get(m);
        if (mstate == PROC_UNUSED || mstate == PROC_ZOMBIE)
            continue;
        if (proc_task_tgid(m) == tgid)
            return 0;
    }
    return 1;
}

/* Detach a zombie: the UNUSED transition is park_lock-owned, the list unlink is
 * tasklist_lock-owned, and tasklist_lock is strictly outside park_lock, so the
 * two nest in that order.  Every caller here already walks the task list (or a
 * children list) under tasklist_lock, which is why this variant takes the outer
 * lock from the caller instead of acquiring it again. */
void proc_reap_detach_list_locked(task_t *t)
{
    if (!t)
        return;
    uint64_t pf = spin_lock_irqsave(&t->park_lock);
    t->state = PROC_UNUSED;
    spin_unlock_irqrestore(&t->park_lock, pf);
    proc_unlink_task_locked(t);
}

/* Is *needle* still reachable from the global task list in a non-UNUSED state?
 * E2: tasklist_lock walks the membership list, and the needle's ->state is read
 * under its own park_lock.  Takes tasklist_lock itself, so the caller must not
 * hold it, and must not hold any task park_lock either, or the sample below
 * would nest two task locks. */
static int proc_task_is_live_locked(task_t *needle)
{
    if (!needle)
        return 0;
    if (proc_task_state_get(needle) == PROC_UNUSED)
        return 0;
    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    int found = 0;
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == needle) {
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, lf);
    return found;
}

/* Requires both the child's and the parent's park_lock held (parent may be
 * NULL).  The public entry points resolve the parent and nest the two locks in
 * INV-P3 order instead of holding one while acquiring the other. */
static int proc_complete_vfork_locked(task_t *child, task_t *parent)
{
    if (!child)
        return 0;

    if (!(child->clone_flags & CLONE_VFORK))
        return 0;

    child->clone_flags &= ~CLONE_VFORK;

#ifdef CONFIG_NOMMU
    if (parent && parent->nommu_vfork_snaps && parent->nommu_num_vfork_snapshots > 0) {
        int n = parent->nommu_num_vfork_snapshots;
        for (int i = 0; i < n; i++) {
            void *dst  = parent->nommu_vfork_snaps[i].dst;
            void *src  = parent->nommu_vfork_snaps[i].data;
            size_t sz  = parent->nommu_vfork_snaps[i].size;
            if (dst && src && sz > 0)
                memcpy(dst, src, sz);
            kfree(src);
            parent->nommu_vfork_snaps[i].data = NULL;
        }
        kfree(parent->nommu_vfork_snaps);
        parent->nommu_vfork_snaps = NULL;
        parent->nommu_num_vfork_snapshots = 0;
    }
#endif

    if (parent)
        parent->vfork_waiting = 0;
    return 1;
}

void proc_complete_vfork(task_t *child)
{
    /* Resolve the parent under tasklist_lock (parent/children chain ownership),
     * then nest {child, parent} park_locks in ascending address order. */
    task_t *parent = NULL;
    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    parent = child ? child->parent : NULL;
    spin_unlock_irqrestore(&tasklist_lock, lf);

    int completed = 0;
    uint64_t cf = 0, pf = 0;
    proc_lock_two_tasks(child, parent, &cf, &pf);
    completed = proc_complete_vfork_locked(child, parent);
    proc_unlock_two_tasks(child, parent, cf, pf);
    if (completed)
        complete(&child->vfork_done);
}

static int proc_child_auto_reaps(task_t *child, task_t *parent)
{
    if (!child)
        return 0;
    if (!parent || parent == proc_idle_task())
        return 1;
    if (child->clone_flags & CLONE_THREAD)
        return !proc_native_thread_waitable(child, parent);
    if (child->exit_signal != SIGCHLD)
        return 0;
    return proc_ignores_sigchld(parent);
}

/* Parent references keep their thread-group leader alive even after list
 * detachment. tasklist_lock protects this group's chain and closes the
 * wait4 scan-to-registration window; each waiter flag/token is sampled under
 * its own park_lock. Never use an unlocked "no waiters" hint here. */
void proc_wake_child_waiters(task_t *parent)
{
    if (!parent)
        return;

    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    task_t *leader = parent->tg_leader ? parent->tg_leader : parent;
    int parent_tgid = proc_task_tgid(parent);
    for (task_t *t = leader; t; t = t->tg_next) {
        uint64_t plf = spin_lock_irqsave(&t->park_lock);
        if (t->waiting_for_child && proc_task_tgid(t) == parent_tgid)
            (void)proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_EVENT);
        spin_unlock_irqrestore(&t->park_lock, plf);
    }
    spin_unlock_irqrestore(&tasklist_lock, lf);
}

/* The outgoing kernel stack is inactive before this notification. A waiter
 * that observed the old CPU slot publishes its park token under tasklist_lock
 * before this notifier can acquire that lock, so the ready event cannot fall
 * between its scan and registration. The caller's pin also retains tg_leader.
 * Reap can win first: UNUSED tasks no longer own a parent/children link, and
 * their stale parent pointer must not be dereferenced. */
void proc_notify_reap_ready(task_t *task)
{
    task_t *parent = NULL, *group_parent = NULL;
    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    if (proc_task_state_get(task) == PROC_ZOMBIE &&
        task->parent != proc_idle_task())
        parent = proc_get(task->parent);
    task_t *leader = task->tg_leader;
    if (leader && leader != task &&
        proc_task_state_get(leader) == PROC_ZOMBIE &&
        proc_tg_group_dead_locked(leader) && leader->parent != proc_idle_task())
        group_parent = proc_get(leader->parent);
    spin_unlock_irqrestore(&tasklist_lock, lf);
    proc_wake_child_waiters(parent);
    if (group_parent != parent)
        proc_wake_child_waiters(group_parent);
    proc_put(group_parent);
    proc_put(parent);
}

/* CPU completion owns these transferred pins. WNOHANG consumers must receive
 * their one exit signal after the reportable child has left every CPU. The
 * early direct wait wake independently overlaps exit and parent dispatch. */
void proc_finish_exit_signal(task_t *task)
{
#ifdef A20_TASK_OBJECTS_ENABLED
    if (task->exit_events_pending) {
        task->exit_events_pending = 0;
        /* The CPU-completion pin also owns task->a20_object until final put,
         * even if a competing reaper has detached its weak task binding. */
        a20_task_object_notify(task, A20_EVENT_EXITED,
                               (uint64_t)task->exit_code, 0, 1);
    }
#endif
    task_t *parent = task->exit_signal_parent;
    task_t *child = task->exit_signal_child;
    int sig = task->exit_signal_pending;
    task->exit_signal_parent = NULL;
    task->exit_signal_child = NULL;
    task->exit_signal_pending = 0;
    if (parent && child && child != task) {
        uint64_t pf = spin_lock_irqsave(&child->park_lock);
        if (proc_task_is_current_any_cpu(child)) {
            /* The last sibling can leave its CPU before the zombie leader.
             * Group notification is claimed exactly once under tasklist_lock.
             * Transfer both pins to the leader; its CPU release takes this
             * same park_lock before publishing the slot as empty. */
            child->exit_signal_parent = parent;
            child->exit_signal_child = child;
            child->exit_signal_pending = sig;
            spin_unlock_irqrestore(&child->park_lock, pf);
            return;
        }
        spin_unlock_irqrestore(&child->park_lock, pf);
    }
    if (parent && sig > 0) {
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
        if (child)
            signal_notify_child_exit(parent, child);
        else
#endif
            signal_send_task(parent, sig);
    }
    proc_put(child);
    proc_put(parent);
}

#ifdef PROC_GROUP_EXIT_NOTIFY_ENABLED
/* Caller holds tasklist_lock. Signal-based parents (e.g. a POSIX shell) may
 * consume SIGCHLD with WNOHANG before the last sibling exits. Publish exactly
 * one notification when the whole group is reportable, and pin its parent
 * across the unlocked wake/signal work. No remote current task is accessed. */
static task_t *proc_group_exit_parent_get_locked(task_t *leader, int *signal) {
    if (!leader || leader->exit_parent_notified ||
        proc_task_state_get(leader) != PROC_ZOMBIE ||
        !proc_tg_group_dead_locked(leader))
        return NULL;
    task_t *parent = leader->parent;
    if (!parent || parent == proc_idle_task() ||
        proc_child_auto_reaps(leader, parent))
        return NULL;
    task_t *referenced = proc_get(parent);
    if (!referenced)
        return NULL;
    leader->exit_parent_notified = 1;
    *signal = leader->exit_signal;
    return referenced;
}
#endif

static void proc_release_exiting_mm(task_t *t)
{
    if (!t)
        return;

    pt_root_t *kernel_pgdir = proc_kernel_pgdir_shared();
    uint64_t kernel_as = kernel_pgdir ? arch_make_addr_space_token(kernel_pgdir) : 0;

    if (t == proc_current() && kernel_as) {
        arch_switch_addr_space_token(kernel_as);
        mm_context_leave(t->mm, cpu_current_id());
    }

    /* ->mm / ->pgdir are published together with the task's scheduling state,
     * so the swap happens under the task's park_lock. */
    uint64_t mm_swap_flags = spin_lock_irqsave(&t->park_lock);
    mm_struct_t *mm = t->mm;
    if (!mm) {
        spin_unlock_irqrestore(&t->park_lock, mm_swap_flags);
        return;
    }
    t->mm = NULL;
    t->pgdir = kernel_pgdir;
    spin_unlock_irqrestore(&t->park_lock, mm_swap_flags);
    if (t->trap_ctx)
        TRAP_CTX_KScratch0(t->trap_ctx) = kernel_as;

    size_t mm_rss = mm_rss_get(mm);
    if (t->cgroup && mm_rss > 0)
        cg_mem_uncharge(t->cgroup, mm_rss);

    /* swap_free() releases the original slot owner's charge at the final
     * reference, including slots shared by forked address spaces. */

    mm_destroy(mm);
}

/* Pick a live thread of *dead*'s group to take over reaping.  Each candidate's
 * ->state is sampled under its own park_lock, one at a time; the caller must
 * not hold a task park_lock (INV-P3). */
/* ---- reparent adapter ---- */

int a20_ex_exit_code(const task_t *t) { return t->exit_code; }
int a20_ex_pid(const task_t *t) { return t->pid; }
int a20_ex_tgid(const task_t *t) { return t->tgid; }
task_t *a20_ex_tg_leader(const task_t *t) { return t->tg_leader; }
task_t *a20_ex_tg_next(const task_t *t) { return t->tg_next; }
int a20_ex_exit_signal(const task_t *t) { return t->exit_signal; }
uint64_t a20_ex_clone_flags(const task_t *t) { return t->clone_flags; }
task_t *a20_ex_children(const task_t *t) { return t->children; }
task_t *a20_ex_sibling_next(const task_t *t) { return t->sibling_next; }
task_t *a20_ex_idle_task(void) { return proc_idle_task(); }
int a20_ex_state(task_t *t) { return proc_task_state_get(t); }
uint64_t a20_ex_park_lock(task_t *t) { return spin_lock_irqsave(&t->park_lock); }
void a20_ex_park_unlock(task_t *t, uint64_t flags) { spin_unlock_irqrestore(&t->park_lock, flags); }
int a20_ex_pdeathsig(const task_t *t) { return t->pdeathsig; }
void a20_ex_set_pdeathsig(task_t *t, int sig) { t->pdeathsig = sig; }
int a20_ex_tg_group_dead(task_t *t) { return proc_tg_group_dead_locked(t); }
int a20_ex_native_thread_waitable(task_t *child, task_t *reaper) {
    return proc_native_thread_waitable(child, reaper);
}
int a20_ex_is_current_any_cpu(task_t *t) {
    return proc_task_is_current_any_cpu(t) ? 1 : 0;
}
void a20_ex_reap_detach(task_t *t) { proc_reap_detach_list_locked(t); }
void a20_ex_note_zombie(void) { proc_sched_note_zombie(); }
task_t *a20_ex_get(task_t *t) { return proc_get(t); }

/*
 * Move the children of @dead under a reaper.  The decision (reaper election,
 * group-dead reaping, pdeathsig claims, reparent target) is
 * Rust's; this adapter performs the deferred calls after the list lock is
 * dropped, in the same order the C implementation used.
 */
static void proc_reparent_children(task_t *dead, task_t *reaper)
{
    exit_reparent_lists_t lists;
    exit_reparent_result_t res;
    a20_ex_reparent_children(dead, reaper, &lists, &res);

    if (res.wake_for)
        proc_wake_child_waiters(res.wake_for);

    for (size_t i = 0; i < res.destroy_count; i++) {
        proc_destroy_task(lists.to_destroy[i]);
        proc_put(lists.to_destroy[i]);
    }

    for (size_t i = 0; i < res.pdeathsig_count; i++) {
        task_t *child = proc_find_get(lists.pdeathsig_pids[i]);
        if (child && child->state != PROC_UNUSED && child->state != PROC_ZOMBIE)
            signal_send(child->pid, lists.pdeathsig_signals[i]);
        proc_put(child);
    }
}

static void proc_clear_child_tid_direct(task_t *t)
{
    if (!t->clear_child_tid)
        return;
    int *ctid = t->clear_child_tid;
    t->clear_child_tid = NULL;

    /* The checked copy is required: it breaks COW before storing. */
    int zero = 0;
    (void)copy_to_user(ctid, &zero, sizeof(zero));
}

typedef struct task_exit_context {
    task_t *task;
    int exit_code;
    int *clear_tid;
    task_t *parent;
    int auto_reap;
    int vfork_completed;
    task_t *thread_leader;
    task_t *completed_parent;
    int completed_signal;
    task_t *notify_leader;
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    struct hyp_bridge_drop hyp_drop;
#endif
} task_exit_context_t;

static task_exit_context_t *exit_ctx(void *opaque)
{
    return (task_exit_context_t *)opaque;
}

static void exit_begin(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    t->exit_pending = 0;
    t->pending_exit_code = ctx->exit_code;
    ktrace_exit("[EXIT] proc_exit: pid=%d tgid=%d thread=%d exit_code=%d\n",
                t->pid, t->tgid,
                (t->clone_flags & CLONE_THREAD) != 0, ctx->exit_code);
}

static void exit_clear_tid(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    ctx->clear_tid = t->clear_child_tid;
    proc_clear_child_tid_direct(t);
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (ctx->clear_tid)
        futex_wake_user(ctx->clear_tid, 1);
#endif
}

static void exit_robust_list_stage(void *opaque)
{
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    task_t *t = exit_ctx(opaque)->task;
    if (t->robust_list_head)
        exit_robust_list(t);
#else
    (void)opaque;
#endif
}

static void exit_release_locks(void *opaque)
{
    task_t *t = exit_ctx(opaque)->task;
    vfs_release_process_locks(t->pid);
    kep_release_process(t->pid);
}

static void exit_account(void *opaque)
{
    acct_task_exit(exit_ctx(opaque)->task);
}

static void exit_close_files(void *opaque)
{
    task_t *t = exit_ctx(opaque)->task;
    ktrace_exit("[EXIT] pid=%d: closing fds and releasing mm ref\n", t->pid);
    fdtable_close_all(t);
}

static void exit_release_mm(void *opaque)
{
    proc_release_exiting_mm(exit_ctx(opaque)->task);
}

static void exit_debug_stop(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    if (proc_debug_is_traced(t) &&
        (t->ptrace_flags & PT_DEBUG_FLAG_TRACEEXIT) &&
        ctx->exit_code != -SIGKILL)
        (void)proc_debug_event_stop(SIGTRAP, PT_DEBUG_EVENT_EXIT,
                                    (uint64_t)(uintptr_t)ctx->exit_code);
}

/* Resolve parent/vfork and retain the thread leader before publishing zombie. */
static void exit_resolve_parent(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    ctx->parent = proc_get(t->parent);
    spin_unlock_irqrestore(&tasklist_lock, lf);
    if (!proc_task_is_live_locked(ctx->parent)) {
        proc_put(ctx->parent);
        ctx->parent = NULL;
    }
    ctx->auto_reap = proc_child_auto_reaps(t, ctx->parent);

    uint64_t cf = 0, pf = 0;
    proc_lock_two_tasks(t, ctx->parent, &cf, &pf);
    ctx->vfork_completed = proc_complete_vfork_locked(t, ctx->parent);
    proc_unlock_two_tasks(t, ctx->parent, cf, pf);
    if (ctx->vfork_completed)
        complete(&t->vfork_done);

    lf = spin_lock_irqsave(&tasklist_lock);
    if (t->tg_leader && t->tg_leader != t)
        ctx->thread_leader = proc_get(t->tg_leader);
    spin_unlock_irqrestore(&tasklist_lock, lf);
}

static void exit_publish_zombie(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    proc_runq_remove_locked(t);
    t->exit_code = ctx->exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state = PROC_ZOMBIE;
    spin_unlock_irqrestore(&t->park_lock, flags);
}

static void exit_parent_bookkeeping(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    uint64_t lf;
#ifdef PROC_GROUP_EXIT_NOTIFY_ENABLED
    int completed_signal = 0;
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    ctx->hyp_drop.count = 0;
#endif
    lf = spin_lock_irqsave(&tasklist_lock);
    task_t *group_leader = ctx->thread_leader ? ctx->thread_leader : t;
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (proc_task_state_get(group_leader) == PROC_ZOMBIE &&
        proc_tg_group_dead_locked(group_leader) &&
        !group_leader->hyp_cleanup_done) {
        group_leader->hyp_cleanup_done = 1;
        hyp_bridge_detach_group(proc_task_tgid(group_leader), &ctx->hyp_drop);
    }
#endif
    ctx->completed_parent = proc_group_exit_parent_get_locked(
        group_leader, &completed_signal);
    ctx->completed_signal = completed_signal;
    if (ctx->completed_parent && completed_signal > 0)
        ctx->notify_leader = proc_get(group_leader);
    spin_unlock_irqrestore(&tasklist_lock, lf);
#else
    if (ctx->thread_leader) {
        lf = spin_lock_irqsave(&tasklist_lock);
        if (proc_task_state_get(ctx->thread_leader) == PROC_ZOMBIE &&
            proc_tg_group_dead_locked(ctx->thread_leader) &&
            ctx->thread_leader->parent &&
            ctx->thread_leader->parent != proc_idle_task())
            ctx->completed_parent = proc_get(ctx->thread_leader->parent);
        spin_unlock_irqrestore(&tasklist_lock, lf);
    }
#endif

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
#ifdef PROC_GROUP_EXIT_NOTIFY_ENABLED
    hyp_bridge_release_group(&ctx->hyp_drop);
#endif
#endif
    ktrace_exit("[EXIT] pid=%d: zombie, auto_reap=%d ctid=%p\n",
                t->pid, ctx->auto_reap, (void *)ctx->clear_tid);
    if (ctx->auto_reap) {
        lf = spin_lock_irqsave(&tasklist_lock);
        proc_sched_note_zombie();
        t->parent = proc_idle_task();
        t->ppid = 0;
        proc_children_unlink_locked(t);
        spin_unlock_irqrestore(&tasklist_lock, lf);
    }
    /* Start the parent's dispatch while this CPU finishes exit. A waiter
     * that still sees CPU ownership requests one completion notification
     * under the child's park_lock before parking, closing the release race. */
    proc_wake_child_waiters(ctx->parent);
    if (ctx->completed_parent != ctx->parent)
        proc_wake_child_waiters(ctx->completed_parent);
#ifndef PROC_GROUP_EXIT_NOTIFY_ENABLED
    proc_put(ctx->completed_parent);
    ctx->completed_parent = NULL;
#endif
    proc_put(ctx->thread_leader);
    ctx->thread_leader = NULL;
    /* Keep ctx->parent pinned until the final notification stage. */
}

static void exit_task_services(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
    /* Release driver registrations before notifying EXITED observers. */
    udriver_task_cleanup(t->pid);
    udisk_task_exit(t->pid);
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    a20_registry_task_exit(t->pid);
#endif
#ifdef A20_TASK_OBJECTS_ENABLED
    /* Native event consumers can use TASK_WAIT_NONBLOCK, just like pidfd /
     * SIGCHLD consumers use WNOHANG. Publish EXITED only after CPU release. */
    t->exit_events_pending = 1;
#else
    a20_event_notify((void *)(uintptr_t)t->pid, A20_OBJ_TASK,
                     A20_EVENT_EXITED, (uint64_t)ctx->exit_code, 0);
    a20_eventq_on_object_destroy((void *)(uintptr_t)t->pid, A20_OBJ_TASK);
#endif
}

static void exit_reparent_children(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *init_reaper = ctx->auto_reap ? NULL : proc_find_get(1);
    proc_reparent_children(ctx->task, init_reaper);
    proc_put(init_reaper);
}

static void exit_tracer_detach(void *opaque)
{
    proc_debug_tracer_exiting(exit_ctx(opaque)->task);
}

static void exit_notify_parent(void *opaque)
{
    task_exit_context_t *ctx = exit_ctx(opaque);
    task_t *t = ctx->task;
#ifdef PROC_GROUP_EXIT_NOTIFY_ENABLED
    if (ctx->completed_parent && ctx->completed_signal > 0) {
        t->exit_signal_parent = ctx->completed_parent;
        t->exit_signal_pending = ctx->completed_signal;
        ctx->completed_parent = NULL;
        t->exit_signal_child = ctx->notify_leader;
        ctx->notify_leader = NULL;
    }
    proc_put(ctx->notify_leader);
    proc_put(ctx->completed_parent);
#else
    if (!ctx->auto_reap && ctx->parent && t->exit_signal > 0) {
        t->exit_signal_parent = ctx->parent;
        t->exit_signal_pending = t->exit_signal;
        ctx->parent = NULL;
    }
#endif
    proc_put(ctx->parent);
    ctx->parent = NULL;
}

static void exit_schedule(void *opaque)
{
    (void)opaque;
    sched();
    panic("proc_exit: sched returned");
}

static void proc_exit_rust(task_t *t, int exit_code)
{
    task_exit_context_t ctx = { .task = t, .exit_code = exit_code };
    const task_exit_ops_t ops = {
        .context = &ctx,
        .begin = exit_begin,
        .clear_tid = exit_clear_tid,
        .robust_list = exit_robust_list_stage,
        .release_locks = exit_release_locks,
        .account = exit_account,
        .close_files = exit_close_files,
        .release_mm = exit_release_mm,
        .debug_stop = exit_debug_stop,
        .resolve_parent = exit_resolve_parent,
        .publish_zombie = exit_publish_zombie,
        .parent_bookkeeping = exit_parent_bookkeeping,
        .task_services = exit_task_services,
        .reparent_children = exit_reparent_children,
        .tracer_detach = exit_tracer_detach,
        .notify_parent = exit_notify_parent,
        .schedule = exit_schedule,
    };
    a20_task_exit_run(&ops);
    panic("proc_exit: Rust controller returned");
}

void proc_exit(int exit_code)
{
    task_t *t = proc_current();
    if (!t)
        panic("proc_exit: no current task");
    cluster_ssi_exit(t);
    proc_exit_rust(t, exit_code);
    panic("proc_exit: returned from Rust path");
}

void proc_force_exit(task_t *t, int exit_code)
{
    if (!t)
        return;
#if defined(CONFIG_KLOG_SESSION) && CONFIG_KLOG_SESSION
    if (exit_code < 0) {
        task_t *sender = proc_current();
        klog_record_only(KLOG_WARN, "[PROCESS] force exit sender=%u name=%s target=%u name=%s status=%d caller=%p\n",
                         sender ? (unsigned)sender->pid : 0,
                         sender ? sender->name : "kernel", (unsigned)t->pid,
                         t->name, exit_code, __builtin_return_address(0));
    }
#endif
    if (t == proc_current())
        proc_exit(exit_code);

    int resume_stopped = 0;
    /* Forced exit touches only the target task's own scheduling and wait
     * bookkeeping, so the target's park_lock is the whole critical section
     * (with the alarm heap and runqueue locks nested inside it). */
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    if (t->state != PROC_UNUSED && t->state != PROC_ZOMBIE) {
        t->pending_exit_code = exit_code;
        __atomic_store_n(&t->exit_pending, 1, __ATOMIC_RELEASE);
        if (t->waiting_for_child)
            t->waiting_for_child = 0;
        if (t->state == PROC_BLOCKED) {
            /*
             * REMOTE_EXIT_SAFE_BOUNDARY: a cancelable Park consumes the task
             * exit reason through its current sequence.  If an event already
             * won, exit_pending remains persistent and is consumed at the
             * next syscall/trap boundary.  Uninterruptible waits are left
             * blocked until their resource event; there is no READY fallback.
             */
        }

        /*
         * REMOTE_EXIT_SAFE_BOUNDARY: always meet a concurrent Park under its
         * lock.  Checking only PROC_BLOCKED misses the PREPARING -> PARKED
         * transition and can leave an exec sibling asleep forever.  The lock
         * is the one already held here, so this is not a nested re-acquire.
         */
        (void)proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_TASK_EXIT);

        if (t->state == PROC_STOPPED) {
            resume_stopped = 1;
        }
    }
    spin_unlock_irqrestore(&t->park_lock, flags);

    if (resume_stopped)
        (void)proc_sched_resume_stopped(t, 0);
}

void proc_exec_terminate_siblings(task_t *self)
{
    if (!self)
        return;

    int self_tgid = proc_task_tgid(self);
    int pids[128];
    int pid_count;
    int active;

    do {
        pid_count = 0;
        active = 0;
        /* Chain membership is tasklist_lock; each sibling's ->state is sampled
         * under its own park_lock, one at a time (no two task locks nested). */
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        task_t *leader = self->tg_leader ? self->tg_leader : self;
        for (task_t *t = leader; t; t = t->tg_next) {
            if (t == self || proc_task_state_get(t) == PROC_UNUSED ||
                proc_task_state_get(t) == PROC_ZOMBIE)
                continue;
            if (proc_task_tgid(t) != self_tgid)
                continue;
            active = 1;
            if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
                continue;
            if (pid_count == (int)(sizeof(pids) / sizeof(pids[0])))
                break;
            pids[pid_count++] = t->pid;
        }
        spin_unlock_irqrestore(&tasklist_lock, flags);

        for (int i = 0; i < pid_count; i++) {
            task_t *sibling = proc_find_get(pids[i]);
            if (sibling) {
                proc_force_exit(sibling, -SIGKILL);
                proc_put(sibling);
            }
        }
        if (active)
            proc_yield();
    } while (active);
}

void proc_exit_group(int exit_code)
{
    task_t *self = proc_current();
    if (!self) {
        proc_exit(exit_code);
        __builtin_unreachable();
    }

    ktrace_exit("[EXIT] exit_group: pid=%d tgid=%d exit_code=%d\n",
                self->pid, self->tgid, exit_code);

    int pids[128];
    int pid_count;
    int self_tgid = proc_task_tgid(self);

    do {
        pid_count = 0;
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        task_t *leader = self->tg_leader ? self->tg_leader : self;
        for (task_t *t = leader; t; t = t->tg_next) {
            if (t == self || proc_task_state_get(t) == PROC_UNUSED ||
                proc_task_state_get(t) == PROC_ZOMBIE)
                continue;
            if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
                continue;
            /*
             * Linux exit_group() targets a thread group, not every task
             * sharing an address space.  A vfork()/posix_spawn() child uses
             * CLONE_VM temporarily but has its own TGID; treating shared mm
             * as group membership would incorrectly terminate its parent.
             */
            if (proc_task_tgid(t) == self_tgid) {
                if (pid_count < (int)(sizeof(pids) / sizeof(pids[0])))
                    pids[pid_count++] = t->pid;
                else
                    break;
            }
        }
        spin_unlock_irqrestore(&tasklist_lock, flags);
        for (int i = 0; i < pid_count; i++) {
            task_t *t = proc_find_get(pids[i]);
            if (t) {
                proc_force_exit(t, exit_code);
                proc_put(t);
            }
        }
    } while (pid_count == (int)(sizeof(pids) / sizeof(pids[0])));
    proc_exit(exit_code);
}

void proc_check_exit_pending(void)
{
    task_t *t = proc_current();
    if (!t)
        return;
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
        proc_exit(t->pending_exit_code);
}
