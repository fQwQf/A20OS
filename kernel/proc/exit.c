#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "bpf/bpf.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/swap.h"
#include "core/panic.h"
#include "core/string.h"
#include "sys/futex.h"
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

void proc_reap_detach_locked(task_t *t)
{
    if (!t)
        return;
    t->state = PROC_UNUSED;
    proc_unlink_task_locked(t);
}

static int proc_task_is_live_locked(task_t *needle)
{
    if (!needle)
        return 0;
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == needle && t->state != PROC_UNUSED)
            return 1;
    }
    return 0;
}

static int proc_complete_vfork_locked(task_t *child)
{
    if (!child)
        return 0;

    if (!(child->clone_flags & CLONE_VFORK))
        return 0;

    child->clone_flags &= ~CLONE_VFORK;
    task_t *parent = child->parent;

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
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int completed = proc_complete_vfork_locked(child);
    spin_unlock_irqrestore(&proc_lock, flags);
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

static void proc_wake_child_waiters_locked(task_t *parent)
{
    if (!parent)
        return;

    int parent_tgid = proc_task_tgid(parent);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (!t->waiting_for_child)
            continue;
        if (proc_task_tgid(t) != parent_tgid)
            continue;
        (void)proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_EVENT);
    }
}

static void proc_release_exiting_mm(task_t *t)
{
    if (!t || !t->mm)
        return;

    mm_struct_t *mm = t->mm;
    pt_root_t *kernel_pgdir = proc_kernel_pgdir_shared();
    uint64_t kernel_as = kernel_pgdir ? arch_make_addr_space_token(kernel_pgdir) : 0;

    if (t == proc_current() && kernel_as) {
        arch_write_addr_space_token(kernel_as);
        arch_tlb_flush();
    }

    t->mm = NULL;
    t->pgdir = kernel_pgdir;
    if (t->trap_ctx)
        TRAP_CTX_KScratch0(t->trap_ctx) = kernel_as;

    if (t->cgroup && mm->rss > 0)
        cg_mem_uncharge(t->cgroup, mm->rss);

    if (t->cgroup && mm->pgdir) {
        size_t swapped = 0;
        for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
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
        if (child->state == PROC_UNUSED ||
            (child->ppid != dead->pid && child->parent != dead)) {
            child = next;
            continue;
        }

        if (!thread_reaper &&
            (actual_reaper == proc_idle_task() ||
             child->exit_signal != SIGCHLD ||
             (child->clone_flags & CLONE_THREAD)) &&
            child->state == PROC_ZOMBIE &&
            !proc_task_is_current_any_cpu(child)) {
            if (destroy_count < (int)(sizeof(to_destroy) / sizeof(to_destroy[0]))) {
                task_t *owned = proc_get(child);
                if (owned) {
                    proc_reap_detach_locked(child);
                    to_destroy[destroy_count++] = owned;
                }
            } else {
                proc_sched_note_zombie();
                child->ppid = actual_reaper->pid;
                child->parent = actual_reaper;
            }

            thread_reaper = proc_find_live_thread_reaper_locked(dead);
            actual_reaper = thread_reaper;
            if (!thread_reaper) {
                actual_reaper = reaper;
                if (!actual_reaper || actual_reaper == dead ||
                    actual_reaper->state == PROC_UNUSED ||
                    actual_reaper->state == PROC_ZOMBIE)
                    actual_reaper = proc_idle_task();
            }
            if (child->state == PROC_UNUSED) {
                child = next;
                continue;
            }
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
        proc_destroy_task(to_destroy[i]);
        proc_put(to_destroy[i]);
    }

    for (int i = 0; i < child_pid_count; i++) {
        task_t *child = proc_find_get(child_pids[i]);
        if (child && child->state != PROC_UNUSED && child->state != PROC_ZOMBIE)
            proc_force_exit(child, dead->exit_code);
        proc_put(child);
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
    t->exit_pending = 0;
    t->pending_exit_code = exit_code;

    ktrace_exit("[EXIT] proc_exit: pid=%d tgid=%d thread=%d exit_code=%d\n",
                t->pid, t->tgid,
                (t->clone_flags & CLONE_THREAD) != 0, exit_code);

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

    ktrace_exit("[EXIT] pid=%d: closing fds and releasing mm ref\n", t->pid);
    fdtable_close_all(t);
    proc_release_exiting_mm(t);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_runq_remove_locked(t);
    task_t *parent = t->parent;
    if (!proc_task_is_live_locked(parent))
        parent = NULL;
    int auto_reap = proc_child_auto_reaps(t, parent);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state = PROC_ZOMBIE;

    ktrace_exit("[EXIT] pid=%d: zombie, auto_reap=%d ctid=%p\n",
                t->pid, auto_reap, (void *)ctid_to_wake);

    int vfork_completed = proc_complete_vfork_locked(t);

    if (auto_reap) {
        proc_sched_note_zombie();
        t->parent = proc_idle_task();
        t->ppid = 0;
    } else {
        proc_wake_child_waiters_locked(parent);
    }
    int notify_parent_pid =
        !auto_reap && parent && t->exit_signal > 0 ? parent->pid : -1;
    spin_unlock_irqrestore(&proc_lock, flags);

    if (vfork_completed)
        complete(&t->vfork_done);

    a20_event_notify(t, A20_OBJ_TASK, 0, (uint64_t)exit_code, 0);

    task_t *init_reaper = auto_reap ? NULL : proc_find_get(1);
    proc_reparent_children(t, init_reaper);
    proc_put(init_reaper);

    if (notify_parent_pid > 0)
        signal_send(notify_parent_pid, t->exit_signal);

    sched();
    panic("proc_exit: sched returned");
}

void proc_force_exit(task_t *t, int exit_code)
{
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;
    if (t == proc_current())
        proc_exit(exit_code);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state != PROC_UNUSED && t->state != PROC_ZOMBIE) {
        t->pending_exit_code = exit_code;
        __atomic_store_n(&t->exit_pending, 1, __ATOMIC_RELEASE);
        if (t->state == PROC_BLOCKED || t->state == PROC_STOPPED) {
            t->waiting_for_child = 0;
            /*
             * Preserve the park token boundary when force-exit races a
             * blocking operation.  The STOPPED fallback has no park token.
             */
            if (!proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_EXIT)) {
                proc_wait_timer_cancel_locked(t, t->wait_seq);
                t->wake_time = 0;
                t->state = PROC_READY;
                proc_runq_enqueue_locked(t);
            }
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
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
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == self || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
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
        spin_unlock_irqrestore(&proc_lock, flags);
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
