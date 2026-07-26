#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/signal.h"
#include "bpf/bpf.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/cpu.h"
#include "core/panic.h"
#include "core/string.h"
#include "cg/cgroup.h"

#ifndef CONFIG_MCU
extern void arch_return_to_user(trap_context_t *ctx) NORETURN;
#endif

task_t *proc_task_alloc_storage(void)
{
    task_t *t = kcalloc(1, sizeof(*t));
    if (!t)
        return NULL;
    refcount_set(&t->refs, 1);
    proc_lifetime_note_task_init(1);
    /* PROC_BLOCKED_ALLOCATION_WHITELIST: unpublished task, no wait token. */
    t->state = PROC_BLOCKED;
    t->dynamic_alloc = 1;
    t->wait_timer_index = -1;
    t->owner_cpu = PROC_CPU_NONE;
    return t;
}

void proc_task_init_idle_state(task_t *t, unsigned cpu)
{
    if (!t)
        return;
    refcount_set(&t->refs, 1);
    proc_lifetime_note_task_init(0);
    t->destroy_started = 0;
    t->state = PROC_RUNNING;
    t->cpu_id = cpu;
    t->on_rq = 0;
    t->dispatching = 0;
    t->on_cpu = 1;
    t->owner_cpu = cpu;
    t->wait_timer_index = -1;
}

void proc_set_name(task_t *t, const char *name)
{
    if (!t)
        return;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

void *proc_scratch_buffer(size_t size)
{
    task_t *t = proc_current();
    if (!t || size == 0)
        return NULL;
    if (t->scratch_buf && t->scratch_size >= size)
        return t->scratch_buf;

    void *buf = kmalloc(size);
    if (!buf)
        return NULL;
    if (t->scratch_buf)
        kfree(t->scratch_buf);
    t->scratch_buf = buf;
    t->scratch_size = size;
    return buf;
}

void proc_task_init_common(task_t *t, task_t *parent)
{
    int parent_pgid = parent ? parent->pgid : 0;
    int parent_sid = parent ? parent->sid : 0;

    t->ppid      = parent ? parent->pid : 0;
    t->tgid      = t->pid;
    t->state     = PROC_READY;
    t->parent    = parent;
    t->exit_code = 0;
    t->priority  = parent ? parent->priority : 0;
    t->sched_level = 0;
    t->cpu_id    = parent ? parent->cpu_id : cpu_current_id();
    t->on_rq     = 0;
    t->dispatching = 0;
    t->on_cpu    = 0;
    t->owner_cpu = PROC_CPU_NONE;
    t->vfork_waiting = 0;
    t->rq_next   = NULL;
    t->rq_prev   = NULL;
    t->wait_next = NULL;
    t->exec_start = 0;
    t->ready_since = 0;
    t->cfs_weight = sched_weight_for_nice(t->priority);
    t->sched_policy = parent ? parent->sched_policy : SCHED_NORMAL;
    t->sched_reset_on_fork = 0;
    t->waiting_for_child = 0;
    t->exit_pending = 0;
    t->pending_exit_code = 0;
    t->stop_report_pending = 0;
    t->continue_report_pending = 0;
    t->wake_time = 0;
    t->wait_seq = 0;
    t->wait_deadline = 0;
    t->wait_timer_index = -1;
    t->park_state = PROC_PARK_IDLE;
    t->wait_mode = PROC_WAIT_UNINTERRUPTIBLE;
    t->wake_reason = PROC_WAKE_NONE;
    completion_init(&t->vfork_done);
    t->alarm_expire = 0;
    t->itimer_real_interval = 0;
    memset(t->itimer_values, 0, sizeof(t->itimer_values));
    t->total_time = 0;
    t->child_utime = 0;
    t->child_stime = 0;
    t->pgid      = parent ? (parent_pgid > 0 ? parent_pgid : parent->pid) : t->pid;
    t->sid       = parent ? (parent_sid > 0 ? parent_sid : parent->pid) : t->pid;
    if (t->pgid <= 0)
        t->pgid = t->pid;
    if (t->sid <= 0)
        t->sid = t->pid;
    t->fs.umask     = parent ? parent->fs.umask : 022;
    t->cred.uid       = parent ? parent->cred.uid : 0;
    t->cred.euid      = parent ? parent->cred.euid : 0;
    t->cred.suid      = parent ? parent->cred.suid : 0;
    t->cred.fsuid     = parent ? parent->cred.fsuid : t->cred.euid;
    t->cred.gid       = parent ? parent->cred.gid : 0;
    t->cred.egid      = parent ? parent->cred.egid : 0;
    t->cred.sgid      = parent ? parent->cred.sgid : 0;
    t->cred.fsgid     = parent ? parent->cred.fsgid : t->cred.egid;
    t->cred.ngroups   = parent ? parent->cred.ngroups : 0;
    if (parent)
        memcpy(t->cred.groups, parent->cred.groups, sizeof(t->cred.groups));
    t->cred.cap_effective = parent ? parent->cred.cap_effective : ~(uint64_t)0;
    t->cred.cap_permitted = parent ? parent->cred.cap_permitted : ~(uint64_t)0;
    t->cred.cap_inheritable = parent ? parent->cred.cap_inheritable : 0;
    t->cred.cap_bounding = parent ? parent->cred.cap_bounding : ~(uint64_t)0;
    t->policy.oom_score_adj = parent ? parent->policy.oom_score_adj : 0;
    t->policy.thp_disabled = parent ? parent->policy.thp_disabled : 0;
    t->clone_flags = 0;
    t->exit_signal = SIGCHLD;
    t->clear_child_tid = NULL;
    t->robust_list_head = 0;
    t->sigaltstack.ss_sp = 0;
    t->sigaltstack.ss_flags = SS_DISABLE;
    t->sigaltstack.ss_size = 0;
    t->sig_handling = 0;
    t->sigsuspend_active = 0;
    t->sigwait_active = 0;
    t->sigwait_mask = 0;
    t->sig_saved_ctx = (trap_context_t){0};
    t->sig_blocked = 0;
    if (parent && parent->signals) {
        signal_state_t *parent_ss = (signal_state_t *)parent->signals;
        uint64_t signal_flags = spin_lock_irqsave(&parent_ss->lock);
        t->sig_blocked = parent->sig_blocked;
        spin_unlock_irqrestore(&parent_ss->lock, signal_flags);
    }
    t->sig_old_blocked = 0;
    t->thread_pending = 0;
    ARCH_TASK_INIT(t);
    t->limits.stack = parent ? parent->limits.stack : USER_STACK_MAX_SIZE;
    t->limits.nofile = parent ? parent->limits.nofile : MAX_FILES;
    t->limits.memlock = parent ? parent->limits.memlock : (64 * 1024);
    t->mm        = NULL;
    t->first_kernel_entry = 0;

    t->cgroup     = parent ? parent->cgroup : NULL;
    t->cpus_allowed = parent ? parent->cpus_allowed
                              : (1U << CONFIG_NR_CPUS) - 1;
    t->cg_throttled = 0;
    t->cg_cpu_start = 0;

    if (parent) {
        memcpy(t->fs.cwd, parent->fs.cwd, MAX_PATH_LEN);
        memcpy(t->fs.root_path, parent->fs.root_path, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
    } else {
        t->fs.cwd[0] = '/';
        t->fs.cwd[1] = '\0';
        t->fs.root_path[0] = '/';
        t->fs.root_path[1] = '\0';
        t->exec_path[0] = '\0';
    }
    if (t->fs.cwd[0] == '\0') {
        t->fs.cwd[0] = '/';
        t->fs.cwd[1] = '\0';
    }
    if (t->fs.root_path[0] == '\0') {
        t->fs.root_path[0] = '/';
        t->fs.root_path[1] = '\0';
    }

#ifndef CONFIG_MCU
    if (parent)
        fdtable_copy(t, parent);
    else
        fdtable_init(t);

    t->signals = kmalloc(sizeof(signal_state_t));
    if (t->signals) {
        if (parent && parent->signals)
            signal_copy((signal_state_t *)parent->signals,
                        (signal_state_t *)t->signals);
        else
            signal_init((signal_state_t *)t->signals);
    }
#else
    t->files = NULL;
    t->signals = NULL;
#endif
}

void proc_task_first_entry(void)
{
    task_t *t = proc_current();
    void (*entry)(void) = t ? (void (*)(void))t->first_kernel_entry : NULL;

    if (t)
        t->first_kernel_entry = 0;
    proc_switch_complete();
#ifndef CONFIG_MCU
    if (t && t->trap_ctx)
        arch_return_to_user(t->trap_ctx);
#endif
    if (!entry)
        panic("proc_task_first_entry: no entry");
    entry();
    proc_exit(0);
}

static void proc_task_release_resources(task_t *t)
{
    if (!t)
        return;

    if (t->cgroup) {
        cg_detach_task(t->cgroup, t->pid);
        t->cgroup = NULL;
    }

    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    bpf_release_process(t->pid);

    if (t->mm) {
        mm_destroy(t->mm);
        t->mm = NULL;
    }
    t->pgdir = NULL;

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

#ifdef CONFIG_NOMMU
    /* Free any pending vfork snapshots (in case process is killed before
     * proc_complete_vfork_locked restores and frees them). */
    if (t->nommu_vfork_snaps) {
        for (int i = 0; i < t->nommu_num_vfork_snapshots; i++) {
            if (t->nommu_vfork_snaps[i].data)
                kfree(t->nommu_vfork_snaps[i].data);
        }
        kfree(t->nommu_vfork_snaps);
        t->nommu_vfork_snaps = NULL;
    }
    t->nommu_num_vfork_snapshots = 0;
#endif

    if (t->kstack) {
        kfree(t->kstack_base);
        t->kstack = 0;
        t->kstack_base = NULL;
    }
}

task_t *proc_get(task_t *t)
{
    if (!t)
        return NULL;
    if (!refcount_inc_not_zero(&t->refs)) {
        proc_lifetime_note_ref_get_failure();
        return NULL;
    }
    proc_lifetime_note_ref_get();
    return t;
}

void proc_put(task_t *t)
{
    if (!t)
        return;

    int old = __atomic_load_n(&t->refs.value, __ATOMIC_RELAXED);
    while (old > 0 &&
           !__atomic_compare_exchange_n(&t->refs.value, &old, old - 1, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
    }
    if (old <= 0) {
        proc_lifetime_note_ref_underflow();
        panic("proc_put: pid=%d reference underflow old=%d", t->pid, old);
    }
    proc_lifetime_note_ref_put();
    if (old != 1)
        return;

    /*
     * Static idle tasks retain their allocation reference for the lifetime of
     * the kernel. Reaching zero indicates an ownership-transfer bug.
     */
    if (!t->dynamic_alloc) {
        proc_lifetime_note_bad_final_put();
        panic("proc_put: static task pid=%d reached zero references", t->pid);
    }
    if (!t->destroy_started) {
        proc_lifetime_note_bad_final_put();
        panic("proc_put: live task pid=%d reached zero references", t->pid);
    }

    proc_task_release_resources(t);
    proc_lifetime_note_task_free();
    memset(t, 0, sizeof(*t));
    kfree(t);
}

void proc_destroy_task(task_t *t)
{
    if (!t)
        return;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->destroy_started) {
        proc_lifetime_note_duplicate_destroy();
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }
    t->destroy_started = 1;
    t->state = PROC_UNUSED;
    proc_wait_timer_cancel_locked(t, t->wait_seq);
    proc_runq_remove_locked(t);
    proc_unlink_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);

    proc_pid_unregister(t);
    /* Drop the allocation/global-list lifetime reference. */
    proc_put(t);
}

void proc_free_pid(int pid)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return;
    proc_destroy_task(t);
    proc_put(t);
}
