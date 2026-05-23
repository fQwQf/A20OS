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
        int *ctid = t->clear_child_tid;
        t->clear_child_tid = NULL;
        if (copy_to_user(ctid, &zero, sizeof(zero)) < 0 && t->pgdir) {
            paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)ctid);
            if (pa) {
                pfn_t pfn = phys_to_pfn(pa);
                if (pfn_valid(pfn)) {
                    int *kv = (int *)((char *)pfn_to_virt(pfn) +
                                      ((uintptr_t)ctid & (PAGE_SIZE - 1)));
                    *kv = 0;
                }
            }
        }
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
        futex_wake_user(ctid, 1);
#endif
    }

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (t->robust_list_head)
        exit_robust_list(t);
#endif

    vfs_release_process_locks(t->pid);
    bpf_release_process(t->pid);

    /* Close all file descriptors BEFORE transitioning to ZOMBIE.
     * This ensures socket close notifications (peer_closed) reach the
     * peer BEFORE wait4() can observe the zombie and report exit status.
     * If we close FDs after ZOMBIE, the parent's wait4() could return
     * before peer sockets are notified, causing the peer to hang. */
    fdtable_close_all(t);
    proc_release_exiting_mm(t);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *parent = t->parent;
    int auto_reap = proc_child_auto_reaps(t, parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    proc_runq_remove_locked(t);
    t->state = PROC_ZOMBIE;

    a20_event_notify(t, A20_OBJ_TASK, 0, (uint64_t)exit_code, 0);

    if (auto_reap) {
        t->parent = proc_idle_task();
        t->ppid = 0;
        spin_unlock_irqrestore(&proc_lock, flags);
    } else {
        proc_wake_child_waiters_locked(parent);
        spin_unlock_irqrestore(&proc_lock, flags);
    }

    if (auto_reap) {
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
    }

    if (!auto_reap) {
        task_t *reaper = proc_find(1);
        proc_reparent_children(t, reaper);
    }

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
        int *ctid = t->clear_child_tid;
        t->clear_child_tid = NULL;
        task_t *saved = proc_set_current(t);
        if (copy_to_user(ctid, &zero, sizeof(zero)) < 0 && t->pgdir) {
            paddr_t pa = pt_translate(t->pgdir, (vaddr_t)(uintptr_t)ctid);
            if (pa) {
                pfn_t pfn = phys_to_pfn(pa);
                if (pfn_valid(pfn)) {
                    int *kv = (int *)((char *)pfn_to_virt(pfn) +
                                      ((uintptr_t)ctid & (PAGE_SIZE - 1)));
                    *kv = 0;
                }
            }
        }
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
        futex_wake_user(ctid, 1);
#endif
        proc_set_current(saved);
    }

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    if (t->robust_list_head) {
        task_t *saved = proc_set_current(t);
        exit_robust_list(t);
        proc_set_current(saved);
    }
#endif

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

    a20_event_notify(t, A20_OBJ_TASK, 0, (uint64_t)exit_code, 0);

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
