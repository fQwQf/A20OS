#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "bpf/bpf.h"
#include "core/cpu.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/vm.h"
#include "core/klog.h"
#include "core/panic.h"
#include "sys/futex.h"
#include "abi/linux/futex.h"
#include "sys/usercopy.h"

static int proc_ignores_sigchld(task_t *parent)
{
    if (!parent || !parent->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)parent->signals;
    return ss->actions[SIGCHLD].sa_handler == SIG_IGN ||
	       (ss->actions[SIGCHLD].sa_flags & SA_NOCLDWAIT);
}

static int proc_task_tgid(task_t *t)
{
    return t ? (t->tgid > 0 ? t->tgid : t->pid) : -1;
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

static void proc_wake_child_waiters_locked(task_t *parent)
{
    if (!parent)
        return;

    int parent_tgid = proc_task_tgid(parent);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state != PROC_BLOCKED || !t->waiting_for_child)
            continue;
        if (proc_task_tgid(t) != parent_tgid)
            continue;
        t->waiting_for_child = 0;
        t->state = PROC_READY;
        proc_runq_enqueue_locked(t);
    }
}

static void proc_release_exiting_mm(task_t *t)
{
    if (!t || !t->mm)
        return;

    mm_struct_t *mm = t->mm;
    uint64_t *kernel_pgdir = proc_kernel_pgdir_shared();
    uint64_t kernel_satp = kernel_pgdir ? arch_make_satp(kernel_pgdir) : 0;

    if (t == proc_current() && kernel_satp) {
        arch_write_satp(kernel_satp);
        arch_tlb_flush();
    }

    t->mm = NULL;
    t->pgdir = kernel_pgdir;
    if (t->trap_ctx)
        TRAP_CTX_KScratch0(t->trap_ctx) = kernel_satp;

    mm_destroy(mm);
}

static task_t *proc_find_live_thread_reaper_locked(task_t *dead)
{
    int dead_tgid = proc_task_tgid(dead);
    if (dead_tgid <= 0)
        return NULL;

    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == dead || t == proc_idle_task())
            continue;
        if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
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

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *thread_reaper = proc_find_live_thread_reaper_locked(dead);
    task_t *actual_reaper = thread_reaper;
    if (!thread_reaper) {
        actual_reaper = reaper;
        if (!actual_reaper || actual_reaper == dead ||
            actual_reaper->state == PROC_UNUSED ||
            actual_reaper->state == PROC_ZOMBIE)
            actual_reaper = proc_idle_task();
    }

    for (task_t *child = proc_first_task_locked(); child; ) {
        task_t *next = proc_next_task_locked(child);
        if (child == proc_idle_task()) {
            child = next;
            continue;
        }
        if (child->state == PROC_UNUSED || child->ppid != dead->pid)
        {
            child = next;
            continue;
        }

        if (!thread_reaper &&
            (actual_reaper == proc_idle_task() ||
             child->exit_signal != SIGCHLD ||
             (child->clone_flags & CLONE_THREAD)) &&
            child->state == PROC_ZOMBIE) {
            spin_unlock_irqrestore(&proc_lock, flags);
            proc_destroy_task(child);
            flags = spin_lock_irqsave(&proc_lock);
            thread_reaper = proc_find_live_thread_reaper_locked(dead);
            actual_reaper = thread_reaper;
            if (!thread_reaper) {
                actual_reaper = reaper;
                if (!actual_reaper || actual_reaper == dead ||
                    actual_reaper->state == PROC_UNUSED ||
                    actual_reaper->state == PROC_ZOMBIE)
                    actual_reaper = proc_idle_task();
            }
            child = next;
            continue;
        }

        child->ppid = actual_reaper->pid;
        child->parent = actual_reaper;
        if (child->state == PROC_ZOMBIE)
            proc_wake_child_waiters_locked(actual_reaper);
        child = next;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
}

void proc_exit(int exit_code)
{
    task_t *t = proc_current();
    if (!t)
        panic("proc_exit: no current task");

    if (t->clear_child_tid) {
        int zero = 0;
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_user(t->clear_child_tid, 1);
        t->clear_child_tid = NULL;
    }

    if (t->robust_list_head)
        exit_robust_list(t);

    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    bpf_release_process(t->pid);
    proc_release_exiting_mm(t);

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *parent = t->parent;
    int auto_reap = proc_child_auto_reaps(t, parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    proc_runq_remove_locked(t);
    t->state = PROC_ZOMBIE;

    if (auto_reap) {
        t->parent = proc_idle_task();
        t->ppid = 0;
    } else {
        proc_wake_child_waiters_locked(parent);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    if (!auto_reap && parent && t->exit_signal > 0)
        signal_send(parent->pid, t->exit_signal);

    sched();
    panic("proc_exit: sched returned");
}

void proc_force_exit(task_t *t, int exit_code)
{
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;
    if (t == proc_current())
        proc_exit(exit_code);

    if (t->clear_child_tid) {
        int zero = 0;
        task_t *saved = proc_set_current(t);
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_user(t->clear_child_tid, 1);
        proc_set_current(saved);
        t->clear_child_tid = NULL;
    }

    if (t->robust_list_head) {
        task_t *saved = proc_set_current(t);
        exit_robust_list(t);
        proc_set_current(saved);
    }

    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    bpf_release_process(t->pid);

    /* Read parent and auto_reap UNDER the lock for consistency */
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *parent = t->parent;
    int auto_reap = proc_child_auto_reaps(t, parent);
    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    proc_runq_remove_locked(t);
    t->state = PROC_ZOMBIE;

    if (auto_reap) {
        t->parent = proc_idle_task();
        t->ppid = 0;
    } else {
        proc_wake_child_waiters_locked(parent);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    proc_release_exiting_mm(t);

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    if (!auto_reap && parent && t->exit_signal > 0)
        signal_send(parent->pid, t->exit_signal);

    /* Auto-reaped: nobody will wait — destroy immediately */
    if (auto_reap) {
        proc_destroy_task(t);
    }
}

void proc_exit_group(int exit_code)
{
    task_t *self = proc_current();
    if (!self) {
        proc_exit(exit_code);
        __builtin_unreachable();
    }

    mm_struct_t *mm = self->mm;
    for (;;) {
        int target_pid = -1;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == self || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
                continue;
            if (mm && t->mm == mm) {
                target_pid = t->pid;
                break;
            }
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        if (target_pid < 0)
            break;
        task_t *t = proc_find(target_pid);
        if (t)
            proc_force_exit(t, exit_code);
    }
    proc_exit(exit_code);
}
