#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "bpf/bpf.h"
#include "core/cpu.h"
#include "core/stdio.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/panic.h"
#include "sys/futex.h"
#include "abi/linux/futex.h"
#include "abi/native/ipc_internal.h"
#include "sys/usercopy.h"
#include "cg/cgroup.h"

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

static void proc_complete_vfork_locked(task_t *child)
{
    if (!child)
        return;

    if (!(child->clone_flags & CLONE_VFORK))
        return;

    child->clone_flags &= ~CLONE_VFORK;
    task_t *parent = child->parent;
    if (parent && parent->state == PROC_BLOCKED) {
        parent->vfork_waiting = 0;
        parent->state = PROC_READY;
        proc_runq_enqueue_locked(parent);
    } else if (parent) {
        parent->vfork_waiting = 0;
    }
}

void proc_complete_vfork(task_t *child)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_complete_vfork_locked(child);
    spin_unlock_irqrestore(&proc_lock, flags);
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

    if (t->cgroup && mm->rss > 0)
        cg_mem_uncharge(t->cgroup, mm->rss);

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

    task_t *to_destroy[64];
    int destroy_count = 0;
    int force_kill_children = (dead->exit_code < 0 &&
        dead->exit_code != -SIGCHLD && dead->exit_code != -SIGSTOP);

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

    int child_pids[64];
    int child_pid_count = 0;

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
            child->state = PROC_UNUSED;
            proc_unlink_task_locked(child);
            if (destroy_count < (int)(sizeof(to_destroy) / sizeof(to_destroy[0])))
                to_destroy[destroy_count++] = child;

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

        if (force_kill_children && child->state != PROC_ZOMBIE) {
            if (child_pid_count < (int)(sizeof(child_pids) / sizeof(child_pids[0])))
                child_pids[child_pid_count++] = child->pid;
        }

        child->ppid = actual_reaper->pid;
        child->parent = actual_reaper;
        if (child->state == PROC_ZOMBIE)
            proc_wake_child_waiters_locked(actual_reaper);
        child = next;
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    for (int i = 0; i < destroy_count; i++) {
        proc_pid_unregister(to_destroy[i]);
        proc_task_release_resources(to_destroy[i]);
        kfree(to_destroy[i]);
    }

    for (int i = 0; i < child_pid_count; i++) {
        task_t *child = proc_find(child_pids[i]);
        if (child && child->state != PROC_UNUSED && child->state != PROC_ZOMBIE)
            proc_force_exit(child, dead->exit_code);
    }
}

static void proc_clear_child_tid_direct(task_t *t)
{
    if (!t->clear_child_tid)
        return;
    int *ctid = t->clear_child_tid;
    t->clear_child_tid = NULL;

    if (!t->pgdir)
        return;

    paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)ctid);
    if (!pa)
        return;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return;
    int *kv = (int *)((char *)pfn_to_virt(pfn) +
                      ((uintptr_t)ctid & (PAGE_SIZE - 1)));
    *kv = 0;
}

void proc_exit(int exit_code)
{
    task_t *t = proc_current();
    if (!t)
        panic("proc_exit: no current task");

    int thread_exit = (t->clone_flags & CLONE_THREAD) != 0;

    int *ctid_to_wake = t->clear_child_tid;
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
    bpf_release_process(t->pid);

    if (!thread_exit) {
        fdtable_close_all(t);
        proc_release_exiting_mm(t);
    }

    proc_runq_remove_locked(t);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *parent = t->parent;
    int auto_reap = proc_child_auto_reaps(t, parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state = PROC_ZOMBIE;

    a20_event_notify(t, A20_OBJ_TASK, 0, (uint64_t)exit_code, 0);

    proc_complete_vfork_locked(t);

    if (auto_reap) {
        t->parent = proc_idle_task();
        t->ppid = 0;
        if (t->signals) {
            signal_state_t *ss = (signal_state_t *)t->signals;
            t->signals = NULL;
            if (refcount_dec_and_test(&ss->refcount))
                kfree(ss);
        }
        if (t->scratch_buf) {
            kfree(t->scratch_buf);
            t->scratch_buf = NULL;
            t->scratch_size = 0;
        }
    } else {
        proc_wake_child_waiters_locked(parent);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    proc_reparent_children(t, auto_reap ? NULL : proc_find(1));

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

    int *ctid_to_wake = t->clear_child_tid;
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
    fdtable_close_all(t);
    bpf_release_process(t->pid);

    proc_runq_remove_locked(t);

    proc_release_exiting_mm(t);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *parent = t->parent;
    int auto_reap = proc_child_auto_reaps(t, parent);
    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state = PROC_ZOMBIE;

    a20_event_notify(t, A20_OBJ_TASK, 0, (uint64_t)exit_code, 0);

    proc_complete_vfork_locked(t);

    if (auto_reap) {
        t->parent = proc_idle_task();
        t->ppid = 0;
    } else {
        proc_wake_child_waiters_locked(parent);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    if (!auto_reap && parent && t->exit_signal > 0)
        signal_send(parent->pid, t->exit_signal);

    /*
     * Do not destroy a remotely forced task inline. It may still be referenced
     * by scheduler or wait queues; the zombie reaper will unlink and free it
     * after the state transition is globally visible.
     */
}

void proc_exit_group(int exit_code)
{
    task_t *self = proc_current();
    if (!self) {
        proc_exit(exit_code);
        __builtin_unreachable();
    }

    int pids[128];
    int pid_count;

    for (;;) {
        pid_count = 0;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == self || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
                continue;
            if (self->mm && t->mm == self->mm) {
                if (pid_count < (int)(sizeof(pids) / sizeof(pids[0])))
                    pids[pid_count++] = t->pid;
            }
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        if (pid_count == 0)
            break;
        for (int i = 0; i < pid_count; i++) {
            task_t *t = proc_find(pids[i]);
            if (t)
                proc_force_exit(t, exit_code);
        }
    }
    proc_exit(exit_code);
}
