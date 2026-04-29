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
#include "sys/usercopy.h"

static int exit_diag_count;

static int proc_ignores_sigchld(task_t *parent)
{
    if (!parent || !parent->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)parent->signals;
    return ss->actions[SIGCHLD].sa_handler == SIG_IGN ||
	       (ss->actions[SIGCHLD].sa_flags & SA_NOCLDWAIT);
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

static void proc_reparent_children(task_t *dead, task_t *reaper)
{
    if (!dead)
        return;
    if (!reaper || reaper == dead)
        reaper = &proc_table[0];

    int wake_reaper = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        if (child->state == PROC_UNUSED || child->ppid != dead->pid)
            continue;
        if (reaper == &proc_table[0] && child->state == PROC_ZOMBIE) {
            proc_destroy_task(child);
            continue;
        }
        child->ppid = reaper->pid;
        child->parent = reaper;
        if (child->state == PROC_ZOMBIE)
            wake_reaper = 1;
    }
    if (wake_reaper && reaper->state == PROC_BLOCKED)
        proc_make_ready(reaper);
}

void proc_exit(int exit_code)
{
    task_t *t = proc_current();
    if (!t)
        panic("proc_exit: no current task");

    if (exit_diag_count < 128) {
        exit_diag_count++;
        kdebug("[WAITDBG] exit pid=%d ppid=%d code=%d state=%d\n",
               t->pid, t->ppid, exit_code, t->state);
    }

    if (t->clear_child_tid) {
        int zero = 0;
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_user(t->clear_child_tid, 1);
        t->clear_child_tid = NULL;
    }

    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    bpf_release_process(t->pid);
    proc_release_exiting_mm(t);

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    task_t *parent = t->parent;
    int auto_reap = proc_ignores_sigchld(parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    {
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        proc_runq_remove_locked(t);
        spin_unlock_irqrestore(&proc_lock, flags);
    }
    t->state = PROC_ZOMBIE;

    if (auto_reap) {
        t->parent = &proc_table[0];
        t->ppid = 0;
    } else if (parent && parent->state == PROC_BLOCKED) {
        proc_make_ready(parent);
    }

    if (!auto_reap && parent)
        signal_send(parent->pid, SIGCHLD);

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

    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    bpf_release_process(t->pid);

    task_t *parent = t->parent;
    int auto_reap = proc_ignores_sigchld(parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_runq_remove_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);
    proc_release_exiting_mm(t);

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    t->state = PROC_ZOMBIE;

    if (auto_reap) {
        t->parent = &proc_table[0];
        t->ppid = 0;
    } else if (parent && parent->state == PROC_BLOCKED) {
        proc_make_ready(parent);
    }
    if (!auto_reap && parent)
        signal_send(parent->pid, SIGCHLD);
}

void proc_exit_group(int exit_code)
{
    task_t *self = proc_current();
    if (!self)
        proc_exit(exit_code);

    mm_struct_t *mm = self->mm;
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *t = &proc_table[i];
        if (t == self || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
            continue;
        if (mm && t->mm == mm)
            proc_force_exit(t, exit_code);
    }
    proc_exit(exit_code);
}
