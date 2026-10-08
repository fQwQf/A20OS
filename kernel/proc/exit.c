#include "proc/proc.h"
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
extern void hyp_bridge_task_exit(int pid);
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
    if (child->clone_flags & CLONE_THREAD)
        return 1;
    if (child->exit_signal != SIGCHLD)
        return 0;
    return proc_ignores_sigchld(parent);
}

/* Wake the tasks blocked in wait4() whose reaper is *parent*.
 *
 * E2: tasklist_lock walks the global list, and each candidate's ->waiting_for_child
 * and ->tgid are park_lock-owned, so the walk nests tasklist_lock -> park_lock.
 * The parent's own tgid is sampled in its own critical section *before* the
 * walk, so no two task park_locks are ever held at once (INV-P3).  It is read
 * under tasklist_lock only as a fast-path guard; the value that decides matches
 * is the sampled one. */
void proc_wake_child_waiters(task_t *parent)
{
    if (!parent)
        return;

    /* Fast path: no task is parked in wait4(), so no waiter can match.  The
     * counter is maintained in lockstep with each waiter's ->waiting_for_child
     * under that waiter's park_lock, so it is only an advisory hint here and is
     * read relaxed. */
    if (!__atomic_load_n(&g_proc_waiting_child_waiter_count, __ATOMIC_RELAXED))
        return;

    int parent_tgid;
    {
        uint64_t pf = spin_lock_irqsave(&parent->park_lock);
        parent_tgid = proc_task_tgid(parent);
        spin_unlock_irqrestore(&parent->park_lock, pf);
    }

    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (!t->waiting_for_child)
            continue;
        if (proc_task_tgid(t) != parent_tgid)
            continue;
        uint64_t plf = spin_lock_irqsave(&t->park_lock);
        if (t->waiting_for_child && proc_task_tgid(t) == parent_tgid)
            (void)proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_EVENT);
        spin_unlock_irqrestore(&t->park_lock, plf);
    }
    spin_unlock_irqrestore(&tasklist_lock, lf);
}

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

    if (t->cgroup && mm->pgdir) {
        size_t swapped = 0;
        for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
            for (vaddr_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
                pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
#ifdef CONFIG_SWAP
                if (pte && pte_is_swap(*pte))
                    swapped++;
#else
                (void)pte;
#endif
            }
        }
        if (swapped)
            cg_mem_swap_uncharge(t, swapped);
    }

    mm_destroy(mm);
}

/* Pick a live thread of *dead*'s group to take over reaping.  Each candidate's
 * ->state is sampled under its own park_lock, one at a time; the caller must
 * not hold a task park_lock (INV-P3). */
static task_t *proc_find_live_thread_reaper_locked(task_t *dead)
{
    int dead_tgid = proc_task_tgid(dead);
    if (dead_tgid <= 0)
        return NULL;

    task_t *leader = dead->tg_leader ? dead->tg_leader : dead;
    for (task_t *t = leader; t; t = t->tg_next) {
        if (t == dead || t == proc_idle_task())
            continue;
        int st = proc_task_state_get(t);
        if (st == PROC_UNUSED || st == PROC_ZOMBIE)
            continue;
        if (proc_task_tgid(t) == dead_tgid)
            return t;
    }
    return NULL;
}

static void proc_reparent_children(task_t *dead, task_t *reaper)
{
    if (!dead)
        return;

    task_t *to_destroy[64];
    int destroy_count = 0;
    int force_kill_children = (dead->exit_code < 0 &&
        dead->exit_code != -SIGCHLD && dead->exit_code != -SIGSTOP);

    /* tasklist_lock covers the children chain and every reparent/unlink in this
     * loop.  Child ->state and ->pdeathsig are park_lock-owned and are read
     * under the child's own park_lock nested inside it (tasklist_lock ->
     * park_lock).  The two helpers that sample several tasks' states run with
     * no task lock held, so no two park_locks are ever nested (INV-P3). */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    task_t *thread_reaper = proc_find_live_thread_reaper_locked(dead);
    task_t *actual_reaper = thread_reaper;
    if (!thread_reaper) {
        actual_reaper = reaper;
        if (!actual_reaper || actual_reaper == dead ||
            proc_task_state_get(actual_reaper) == PROC_UNUSED ||
            proc_task_state_get(actual_reaper) == PROC_ZOMBIE)
            actual_reaper = proc_idle_task();
    }

    int child_pids[64];
    int child_pid_count = 0;
    int pdeathsig_pids[64];
    int pdeathsig_signals[64];
    int pdeathsig_count = 0;

    task_t *wake_for = NULL;
    for (task_t *child = dead->children; child; ) {
        task_t *next = child->sibling_next;
        if (child == proc_idle_task()) {
            child = next;
            continue;
        }
        int cstate = proc_task_state_get(child);
        if (cstate == PROC_UNUSED) {
            child = next;
            continue;
        }

        /* PR_SET_PDEATHSIG: when the parent dies, the child receives the
         * recorded signal exactly once (the field is then cleared).  Deliver
         * it after the lock is dropped, like the force-kill list below.
         * ->pdeathsig is park_lock-owned, so the claim is a critical section. */
        uint64_t pf = spin_lock_irqsave(&child->park_lock);
        int pdeathsig = (cstate != PROC_ZOMBIE) ? child->pdeathsig : 0;
        if (pdeathsig &&
            pdeathsig_count < (int)(sizeof(pdeathsig_pids) /
                                    sizeof(pdeathsig_pids[0]))) {
            pdeathsig_pids[pdeathsig_count] = child->pid;
            pdeathsig_signals[pdeathsig_count] = pdeathsig;
            child->pdeathsig = 0;
            pdeathsig_count++;
        }
        spin_unlock_irqrestore(&child->park_lock, pf);

        int reap_group_dead = 0;
        if (!thread_reaper)
            reap_group_dead = proc_tg_group_dead_locked(child);
        if (reap_group_dead &&
            (actual_reaper == proc_idle_task() ||
             child->exit_signal != SIGCHLD ||
             (child->clone_flags & CLONE_THREAD)) &&
            proc_task_state_get(child) == PROC_ZOMBIE &&
            !proc_task_is_current_any_cpu(child)) {
            if (destroy_count < (int)(sizeof(to_destroy) / sizeof(to_destroy[0]))) {
                task_t *owned = proc_get(child);
                if (owned) {
                    proc_reap_detach_list_locked(child);
                    to_destroy[destroy_count++] = owned;
                }
            } else {
                proc_sched_note_zombie();
                proc_reparent_task_locked(actual_reaper, child);
            }

            thread_reaper = proc_find_live_thread_reaper_locked(dead);
            actual_reaper = thread_reaper;
            if (!thread_reaper) {
                actual_reaper = reaper;
                if (!actual_reaper || actual_reaper == dead ||
                    proc_task_state_get(actual_reaper) == PROC_UNUSED ||
                    proc_task_state_get(actual_reaper) == PROC_ZOMBIE)
                    actual_reaper = proc_idle_task();
            }
            if (proc_task_state_get(child) == PROC_UNUSED) {
                child = next;
                continue;
            }
        }

        cstate = proc_task_state_get(child);
        if (force_kill_children && cstate != PROC_ZOMBIE) {
            if (child_pid_count < (int)(sizeof(child_pids) / sizeof(child_pids[0])))
                child_pids[child_pid_count++] = child->pid;
        }

        proc_reparent_task_locked(actual_reaper, child);
        if (cstate == PROC_ZOMBIE && !wake_for)
            wake_for = actual_reaper;
        child = next;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);

    /* The waiter scan re-acquires tasklist_lock, so it runs after the loop. */
    if (wake_for)
        proc_wake_child_waiters(wake_for);

    for (int i = 0; i < destroy_count; i++) {
        proc_destroy_task(to_destroy[i]);
        proc_put(to_destroy[i]);
    }

    for (int i = 0; i < child_pid_count; i++) {
        task_t *child = proc_find_get(child_pids[i]);
        if (child && child->state != PROC_UNUSED && child->state != PROC_ZOMBIE)
            proc_force_exit(child, dead->exit_code);
        proc_put(child);
    }

    for (int i = 0; i < pdeathsig_count; i++) {
        task_t *child = proc_find_get(pdeathsig_pids[i]);
        if (child && child->state != PROC_UNUSED && child->state != PROC_ZOMBIE)
            signal_send(child->pid, pdeathsig_signals[i]);
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

void proc_exit(int exit_code)
{
    task_t *t = proc_current();
    if (!t)
        panic("proc_exit: no current task");
    t->exit_pending = 0;
    t->pending_exit_code = exit_code;

    ktrace_exit("[EXIT] proc_exit: pid=%d tgid=%d thread=%d exit_code=%d\n",
                t->pid, t->tgid,
                (t->clone_flags & CLONE_THREAD) != 0, exit_code);

    int *ctid_to_wake = t->clear_child_tid;
    (void)ctid_to_wake; /* futex wake below is Linux-ABI gated */
    proc_clear_child_tid_direct(t);
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (ctid_to_wake)
        futex_wake_user(ctid_to_wake, 1);
#endif

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (t->robust_list_head)
        exit_robust_list(t);
#endif

    vfs_release_process_locks(t->pid);
    kep_release_process(t->pid);

    acct_task_exit(t);

    ktrace_exit("[EXIT] pid=%d: closing fds and releasing mm ref\n", t->pid);
    fdtable_close_all(t);
    proc_release_exiting_mm(t);

    /*
     * PT_DEBUG_EXIT_STOP: a traced task with the TRACEEXIT option reports a
     * PTRACE_EVENT_EXIT stop before becoming a zombie; the tracer reads the
     * exit code from the event message and resumes the task to complete the
     * exit.  SIGKILL exits are never stopped (kill -9 must always work).
     */
    if (proc_debug_is_traced(t) &&
        (t->ptrace_flags & PT_DEBUG_FLAG_TRACEEXIT) &&
        exit_code != -SIGKILL)
        (void)proc_debug_event_stop(SIGTRAP, PT_DEBUG_EVENT_EXIT,
                                    (uint64_t)(uintptr_t)exit_code);

    /*
     * Three phases, each holding at most one lock class at a time:
     *
     *  1. tasklist_lock resolves the parent and samples its liveness.  Done
     *     first, and separately, so the ZOMBIE publication below never nests
     *     {t, parent} park_locks (INV-P3).  proc_complete_vfork() needs the
     *     parent too and is called while no lock of ours is held.
     *  2. t->park_lock publishes ->state / ->exit_code and drops the task from
     *     the runqueue (park_lock -> runq_lock).
     *  3. tasklist_lock rewrites the parent/children chains for auto-reap.
     *
     * The waiter wake is deferred to phase 3+ because it re-acquires
     * tasklist_lock on its own.
     */
    task_t *parent = NULL;
    uint64_t lf = spin_lock_irqsave(&tasklist_lock);
    parent = t->parent;
    spin_unlock_irqrestore(&tasklist_lock, lf);
    if (!proc_task_is_live_locked(parent))
        parent = NULL;
    int auto_reap = proc_child_auto_reaps(t, parent);

    uint64_t cf = 0, pf = 0;
    proc_lock_two_tasks(t, parent, &cf, &pf);
    int vfork_completed = proc_complete_vfork_locked(t, parent);
    proc_unlock_two_tasks(t, parent, cf, pf);
    if (vfork_completed)
        complete(&t->vfork_done);

    /* Keep a thread group's leader alive across the zombie publication below.
     * Once this member becomes zombie, a concurrent waiter may observe the
     * whole group as dead and reap the leader before we can inspect it again. */
    task_t *thread_leader = NULL;
    lf = spin_lock_irqsave(&tasklist_lock);
    if (t->tg_leader && t->tg_leader != t)
        thread_leader = proc_get(t->tg_leader);
    spin_unlock_irqrestore(&tasklist_lock, lf);

    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    proc_runq_remove_locked(t);
    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state = PROC_ZOMBIE;
    spin_unlock_irqrestore(&t->park_lock, flags);

    /* A thread-group leader may already be a zombie while sibling threads
     * keep the process unreportable to wait4.  The last member's exit makes
     * that leader reportable, so wake the leader's parent after checking the
     * group under tasklist_lock and taking references for the unlocked wake. */
    task_t *completed_parent = NULL;
    if (thread_leader) {
        lf = spin_lock_irqsave(&tasklist_lock);
        if (proc_task_state_get(thread_leader) == PROC_ZOMBIE &&
            proc_tg_group_dead_locked(thread_leader) &&
            thread_leader->parent &&
            thread_leader->parent != proc_idle_task())
            completed_parent = proc_get(thread_leader->parent);
        spin_unlock_irqrestore(&tasklist_lock, lf);
    }

    ktrace_exit("[EXIT] pid=%d: zombie, auto_reap=%d ctid=%p\n",
                t->pid, auto_reap, (void *)ctid_to_wake);

    if (auto_reap) {
        lf = spin_lock_irqsave(&tasklist_lock);
        proc_sched_note_zombie();
        t->parent = proc_idle_task();
        t->ppid = 0;
        proc_children_unlink_locked(t);
        spin_unlock_irqrestore(&tasklist_lock, lf);
    } else {
        proc_wake_child_waiters(parent);
    }
    if (completed_parent)
        proc_wake_child_waiters(completed_parent);
    proc_put(completed_parent);
    proc_put(thread_leader);
    int notify_parent_pid =
        !auto_reap && parent && t->exit_signal > 0 ? parent->pid : -1;

    if (vfork_completed)
        complete(&t->vfork_done);

    /* Release user-space driver IRQ registrations BEFORE the EXITED
     * event fires, so a supervisor reacting to that event can immediately
     * re-register the device (reap-time cleanup would race the respawn). */
    udriver_task_cleanup(t->pid);
    udisk_task_exit(t->pid);
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    a20_registry_task_exit(t->pid);
#endif
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    hyp_bridge_task_exit(t->pid);
#endif

    /* Native ABI task handles store the pid as the object pointer, so the
     * watch key must be pid-as-pointer (docs/native-abi/05-ipc.md §3.3). */
    a20_event_notify((void *)(uintptr_t)t->pid, A20_OBJ_TASK,
                     A20_EVENT_EXITED, (uint64_t)exit_code, 0);
    a20_eventq_on_object_destroy((void *)(uintptr_t)t->pid, A20_OBJ_TASK);

    task_t *init_reaper = auto_reap ? NULL : proc_find_get(1);
    proc_reparent_children(t, init_reaper);
    proc_put(init_reaper);

    /* Detach tracees observing the dying task; EXITKILL tracees are
     * terminated, stopped tracees are resumed. */
    proc_debug_tracer_exiting(t);

    if (notify_parent_pid > 0)
        signal_send(notify_parent_pid, t->exit_signal);

    sched();
    panic("proc_exit: sched returned");
}

void proc_force_exit(task_t *t, int exit_code)
{
    if (!t)
        return;
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
        /* Keep the wait4 aggregate exactly paired with the guarded flag.
         * The former unconditional decrement for every BLOCKED task let an
         * unrelated forced exit hide real child waiters, so later child exits
         * skipped their wake scan and left zombie children behind forever. */
        if (t->waiting_for_child) {
            t->waiting_for_child = 0;
            if (g_proc_waiting_child_waiter_count)
                g_proc_waiting_child_waiter_count--;
        }
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
