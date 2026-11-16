#include "cluster/ssi.h"
#include "proc/proc.h"
#include "fs/tty_control.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/task_object.h"
#ifdef CONFIG_TASK_EXIT_RUST
#include "proc/task_exit.h"
#endif
#ifdef CONFIG_TASK_CONSTRUCTION_TEST
#include "proc/task_construction.h"
#endif
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
#include "ipc/envelope_acquire_race_test.h"
#endif
#include "proc/signal.h"
#include "ipc/seccomp.h"
#include "drivers/core/udriver.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/cpu.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "core/lock_counters.h"
#include "cg/cgroup.h"
#include "ipc/keyring.h"
#include "ipc/landlock.h"
#include "ipc/envelope.h"
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
#include "ipc/envelope_acquire_race_test.h"
#endif
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/handle_table.h"
#endif

#ifndef CONFIG_MCU
extern void arch_return_to_user(trap_context_t *ctx) NORETURN;
#endif

#ifdef CONFIG_DEBUG_BOOT_TRACE
static void dump_user_pt_path(pt_root_t *root, vaddr_t va,
                               const char *name)
{
    pte_t *table = root;

    kinfo("[PT] %s va=0x%lx root=%p root_pa=0x%lx\n", name,
          (unsigned long)va, root,
          (unsigned long)arch_make_addr_space_token(root));
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        pte_t pte = table[idx];

        kinfo("[PT] %s L%d[%d] table=%p pte=0x%lx\n", name, level,
              idx, table, (unsigned long)pte);
        if (level == 0 || !(pte & PTE_V) || arch_pte_is_leaf(pte))
            break;
        table = arch_pte_to_ptr(pte);
    }
}
#endif

/* Task objects are freed, while the counter registry keeps pointers forever.
 * Aggregate park contention into one static sink, including callsite samples. */
static spinlock_t park_counters = SPINLOCK_INIT;

task_t *proc_task_alloc_storage(void)
{
    task_t *t = kcalloc(1, sizeof(*t));
    if (!t)
        return NULL;
    refcount_set(&t->refs, 1);
    proc_lifetime_note_task_init(1);
    /* PROC_BLOCKED_ALLOCATION_WHITELIST: unpublished task, no wait token. */
    t->state = PROC_BLOCKED;
    t->lifecycle_phase = PROC_TASK_PHASE_ALLOCATED;
    t->dynamic_alloc = 1;
    t->wait_timer_index = -1;
    t->alarm_timer_index = -1;
    t->owner_cpu = PROC_CPU_NONE;
    /* The task remains private until its complete task image is ready for the
     * global list. Initialize the lock before any publication; task_init must
     * never reset an observable lock. */
    spin_init(&t->park_lock);
    t->park_lock.counter_sink = &park_counters;
    spin_init(&t->eevdf_deadline_lock);
    spin_init(&t->jobctl_lock);
    return t;
}

void proc_task_park_counters_init(void)
{
    lock_counters_register(&park_counters, "park_lock");
    lock_counters_enable_callsite(&park_counters);
}

void proc_task_init_idle_state(task_t *t, unsigned cpu)
{
    if (!t)
        return;
    spin_init(&t->park_lock);
    t->park_lock.counter_sink = &park_counters;
    spin_init(&t->eevdf_deadline_lock);
    spin_init(&t->jobctl_lock);
    refcount_set(&t->refs, 1);
    proc_lifetime_note_task_init(0);
    t->destroy_started = 0;
    /* Static idle tasks bypass dynamic construction/publication, but their
     * lifecycle phase is already committed so an accidental ready request
     * cannot observe the zero-initialized phase. */
    t->lifecycle_phase = PROC_TASK_PHASE_RUNNABLE;
    t->state = PROC_RUNNING;
    t->cpu_id = cpu;
    t->on_rq = 0;
    t->dispatching = 0;
    t->on_cpu = 1;
    t->owner_cpu = cpu;
    t->wait_timer_index = -1;
    t->alarm_timer_index = -1;
    proc_park_wait_state_init(t);
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

void proc_task_init_base(task_t *t, task_t *parent)
{
    if (proc_task_lifecycle_advance(t, PROC_TASK_EVENT_BEGIN_INIT) < 0)
        panic("task_init: invalid lifecycle phase pid=%d phase=%u",
              t->pid, t->lifecycle_phase);
    int parent_pgid = parent ? parent->pgid : 0;
    int parent_sid = parent ? parent->sid : 0;

    t->ppid      = parent ? parent->pid : 0;
    t->tgid      = t->pid;
    /* A constructed task becomes READY only at proc_make_ready(), after its
     * complete context has been published and all list visibility is stable. */
    t->state     = PROC_BLOCKED;
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
    t->rt_node.next = NULL;
    t->rt_node.prev = NULL;
    t->rt_node.queued_slot = -1;
    t->rt_node.policy = 0;
    t->rt_rr_remaining = 0;
    t->rt_rr_last_account = 0;
    t->wait_next = NULL;
    t->exec_start = 0;
    t->ready_since = 0;
    t->cfs_weight = sched_weight_for_nice(t->priority);
    t->eevdf_vruntime = 0;
    t->eevdf_deadline = 0;
    t->eevdf_last_account = 0;
    t->sched_policy = parent ? parent->sched_policy : SCHED_NORMAL;
    /* A deadline task's child starts in the fair class: inheriting the
     * parent's admission without a fresh bandwidth check would double-book
     * the CPU and leave the child with no period/runtime state. */
    if (t->sched_policy == SCHED_DEADLINE)
        t->sched_policy = SCHED_NORMAL;
    t->sched_reset_on_fork = 0;
    t->dl_runtime = 0;
    t->dl_deadline = 0;
    t->dl_period = 0;
    t->dl_remaining = 0;
    t->dl_period_start = 0;
    t->dl_next_release = 0;
    t->dl_throttled = 0;
    t->dl_admitted = 0;
    t->dl_overrun = 0;
    t->dl_ready_next = NULL;
    t->dl_throttle_next = NULL;
    t->suspend_pending = 0;
    t->cmsis_suspend = 0;
    t->pi_boost_weight = 0;
    t->pi_boosted = 0;
    t->pi_rt_priority = 0;
    t->pi_saved_policy = SCHED_NORMAL;
    t->pi_saved_priority = 0;
    t->pi_saved_nice = 0;
    t->pi_saved_reset_on_fork = 0;
    spin_init(&t->pi_lock);
    t->sched_lock_depth = 0;
    t->fault_storm_pc = 0;
    t->fault_storm_va = 0;
    t->fault_storm_cause = 0;
    t->fault_storm_start = 0;
    t->fault_storm_count = 0;
    t->waiting_for_child = 0;
    t->reap_wait_requested = 0;
    t->exit_signal_parent = NULL;
    t->exit_signal_child = NULL;
    t->exit_signal_pending = 0;
    t->exit_events_pending = 0;
    t->exit_pending = 0;
    t->pending_exit_code = 0;
    t->stop_report_pending = 0;
    t->continue_report_pending = 0;
    t->wake_time = 0;
    t->wait_timer_index = -1;
    t->alarm_timer_index = -1;
    proc_park_wait_state_init(t);
    completion_init(&t->vfork_done);
    t->alarm_expire = 0;
    t->itimer_real_interval = 0;
    memset(t->itimer_values, 0, sizeof(t->itimer_values));
    t->total_time = 0;
    t->utime_ticks = 0;
    t->stime_ticks = 0;
    {
        uint64_t mono[2];
        timekeeping_get_monotonic(mono);
        t->start_jiffies = mono[0] * 100 + mono[1] / 10000000ULL;
    }
    t->child_utime = 0;
    t->child_stime = 0;
    t->pgid      = parent ? (parent_pgid > 0 ? parent_pgid : parent->pid) : t->pid;
    t->sid       = parent ? (parent_sid > 0 ? parent_sid : parent->pid) : t->pid;
    if (t->pgid <= 0)
        t->pgid = t->pid;
    if (t->sid <= 0)
        t->sid = t->pid;
    tty_control_inherit(t, parent);
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
    t->policy.no_new_privs = parent ? parent->policy.no_new_privs : 0;
    t->clone_flags = 0;
    t->exit_signal = SIGCHLD;
#ifdef PROC_GROUP_EXIT_NOTIFY_ENABLED
    t->exit_parent_notified = 0;
#endif
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
#ifdef A20_TASK_OBJECTS_ENABLED
    t->a20_signal_pending = 0;
    t->a20_signal_blocked = parent ? a20_thread_sig_mask(parent, 0, 1) : 0;
#endif
    t->thread_signal_info = NULL;
    ARCH_TASK_INIT(t);
    t->limits.stack = parent ? parent->limits.stack : USER_STACK_MAX_SIZE;
    t->limits.nofile = parent ? parent->limits.nofile : MAX_FILES;
    t->limits.memlock = parent ? parent->limits.memlock : (64 * 1024);
    /* Inherit the parent's limits. AS still uses the legacy zero sentinel;
     * NPROC uses UINT64_MAX so an explicit Linux zero remains a real limit.
     * NPROC is enforced at fork time against a uid-wide count. */
    t->limits.as = parent ? parent->limits.as : 0;
    t->limits.nproc = parent ? parent->limits.nproc : UINT64_MAX;
    t->limits.rtprio = 0;
    t->limits.rtprio_max = 0;
    if (parent) {
        uint64_t pf = spin_lock_irqsave(&parent->park_lock);
        t->limits.rtprio = parent->limits.rtprio;
        t->limits.rtprio_max = parent->limits.rtprio_max;
        spin_unlock_irqrestore(&parent->park_lock, pf);
    }
    t->mm        = NULL;
    t->first_kernel_entry = 0;

    t->cgroup     = NULL;
    t->cpus_allowed = parent ? parent->cpus_allowed
                              : (1U << CONFIG_NR_CPUS) - 1;
    t->cg_throttled = 0;
    t->cg_cpu_start = 0;
    t->files = NULL;
    t->signals = NULL;
}

void proc_task_init_seccomp(task_t *t, task_t *parent)
{
    if (t) {
        seccomp_inherit(t, parent);
        landlock_inherit_task(t, parent);
    }
}

void proc_task_init_fs_cgroup(task_t *t, task_t *parent)
{
    if (!t)
        return;
    t->cgroup = parent ? parent->cgroup : NULL;
    if (parent) {
        memcpy(t->fs.cwd, parent->fs.cwd, MAX_PATH_LEN);
        memcpy(t->fs.root_path, parent->fs.root_path, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
        /* The child gets its own references, not aliases: parent and child
         * chdir and pivot_root independently from here on, and each exit
         * path releases its own pins. */
        vfs_task_fs_pins_copy(t, parent);
        cg_inherit_task(t, parent);
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
}

void proc_task_init_files(task_t *t, task_t *parent, uint64_t clone_flags)
{
    if (!t)
        return;
#ifndef CONFIG_MCU
    if (parent && (clone_flags & (CLONE_FILES | CLONE_THREAD)))
        fdtable_share(t, parent);
    else if (parent)
        fdtable_copy(t, parent);
    else
        fdtable_init(t);
#else
    t->files = NULL;
    (void)parent;
    (void)clone_flags;
#endif
}

int proc_task_init_signals(task_t *t, task_t *parent, uint64_t clone_flags,
                           int checked)
{
    if (!t)
        return -EINVAL;
#ifndef CONFIG_MCU
    if (parent && parent->signals && (clone_flags & CLONE_SIGHAND)) {
        t->signals = parent->signals;
        refcount_inc(&t->signals->refcount);
        return 0;
    }
    t->signals = kmalloc(sizeof(signal_state_t));
    if (!t->signals)
        return checked ? -ENOMEM : 0;
    if (parent && parent->signals)
        signal_copy((signal_state_t *)parent->signals,
                    (signal_state_t *)t->signals);
    else
        signal_init((signal_state_t *)t->signals);
#else
    (void)parent;
    (void)clone_flags;
    (void)checked;
    t->signals = NULL;
#endif
    return 0;
}

static void task_init(task_t *t, task_t *parent, uint64_t clone_flags,
                      int detached, int defer_files)
{
    proc_task_init_base(t, parent);
    proc_task_init_seccomp(t, parent);
    proc_task_init_fs_cgroup(t, parent);
#ifndef CONFIG_MCU
    if (detached) {
        t->files = NULL;
        t->signals = NULL;
        return;
    }
    if (!defer_files)
        proc_task_init_files(t, parent, clone_flags);
#else
    (void)detached;
    (void)defer_files;
#endif
    (void)proc_task_init_signals(t, parent, clone_flags, 0);
}

void proc_task_init_common(task_t *t, task_t *parent, uint64_t clone_flags)
{
    task_init(t, parent, clone_flags, 0, 0);
}

void proc_task_init_common_deferred_files(task_t *t, task_t *parent,
                                          uint64_t clone_flags)
{
    task_init(t, parent, clone_flags, 0, 1);
}

void proc_task_init_detached(task_t *t)
{
    task_init(t, NULL, 0, 1, 0);
}

void proc_task_first_entry(void)
{
    task_t *t = proc_current();
    void (*entry)(void) = t ? (void (*)(void))t->first_kernel_entry : NULL;

#ifdef CONFIG_DEBUG_BOOT_TRACE
    kinfo("[SCHED] first task entry: pid=%d trap=%p entry=%p\n",
          t ? t->pid : -1, t ? (void *)t->trap_ctx : NULL, (void *)entry);
#endif

    if (t)
        t->first_kernel_entry = 0;
    /* A brand-new task reaches user through this path, not through the
     * post-__switch continuation, so publish its thread-pointer bases here
     * as well (no-op on architectures that carry them in registers). */
    arch_switch_tls_restore();
    proc_switch_complete();
#ifndef CONFIG_MCU
    if (t && t->trap_ctx) {
#ifdef CONFIG_DEBUG_BOOT_TRACE
        arch_debug_dump_user_state();
        dump_user_pt_path(t->pgdir, TRAP_CTX_EPC(t->trap_ctx), "entry");
        dump_user_pt_path(t->pgdir, TRAP_CTX_SP(t->trap_ctx) - 1, "stack");
#endif
#ifdef CONFIG_CLUSTER_SSI
        arch_local_irq_disable();
#endif
        cluster_ssi_before_user(t->trap_ctx);
        arch_return_to_user(t->trap_ctx);
    }
#endif
    if (!entry)
        panic("proc_task_first_entry: no entry");
    /* A fresh kernel task has no sched() continuation to restore its saved
     * IRQ state. Leave the switch window before enabling interrupts, or a
     * LoongArch worker can run forever with CRMD.IE clear and block remote
     * TLB acknowledgements. Physical cooperative bring-up keeps its existing
     * no-interrupt contract, just as syscall dispatch does. */
#ifndef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    arch_local_irq_enable();
#endif
    entry();
    proc_exit(0);
}

#ifdef CONFIG_TASK_EXIT_RUST
static task_t *release_task(void *opaque)
{
    return (task_t *)opaque;
}

static void release_cgroup(void *opaque)
{
    task_t *t = release_task(opaque);
    if (t->cgroup) {
        cg_detach_task(t);
        t->cgroup = NULL;
    }
}

static void release_udriver(void *opaque)
{
    task_t *t = release_task(opaque);
    udriver_task_cleanup(t->pid);
}

static void release_process_locks(void *opaque)
{
    task_t *t = release_task(opaque);
    vfs_release_process_locks(t->pid);
}

static void release_files(void *opaque)
{
    fdtable_close_all(release_task(opaque));
}

static void release_mm(void *opaque)
{
    task_t *t = release_task(opaque);
    if (t->mm) {
        mm_destroy(t->mm);
        t->mm = NULL;
    }
    t->pgdir = NULL;
}

static void release_arch(void *opaque)
{
#ifdef ARCH_TASK_RELEASE
    ARCH_TASK_RELEASE(release_task(opaque));
#else
    (void)opaque;
#endif
}

static void release_keyring(void *opaque)
{
    keyring_release_task(release_task(opaque));
}

static void release_landlock(void *opaque)
{
    landlock_release_task(release_task(opaque));
}

static void release_envelope(void *opaque)
{
    env_release_task(release_task(opaque));
}

static void release_thread_signal_info(void *opaque)
{
    task_t *t = release_task(opaque);
    kfree(t->thread_signal_info);
    t->thread_signal_info = NULL;
}

static void release_signal_state(void *opaque)
{
    task_t *t = release_task(opaque);
    if (t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        t->signals = NULL;
        if (refcount_dec_and_test(&ss->refcount))
            kfree(ss);
    }
}

static void release_scratch(void *opaque)
{
    task_t *t = release_task(opaque);
    void *scratch = __atomic_exchange_n(&t->scratch_buf, NULL, __ATOMIC_ACQ_REL);
    /* scratch_buf is Linux ABI transient I/O storage, not Native handles. */
    if (scratch) {
        kfree(scratch);
        t->scratch_size = 0;
    }
}

static void release_handle_table(void *opaque)
{
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    task_t *t = release_task(opaque);
    /* The Native handle table is distinct from scratch_buf and can be shared
     * by a thread group; its own refcount decides the final table release. */
    void *ht = __atomic_exchange_n(&t->a20_ht, NULL, __ATOMIC_ACQ_REL);
    if (ht)
        a20_ht_put_ref((struct a20_ht_internal *)ht);
#else
    (void)opaque;
#endif
}

static void release_nommu_vfork(void *opaque)
{
#ifdef CONFIG_NOMMU
    task_t *t = release_task(opaque);
    /* Final task release is a second cleanup path if vfork exited abnormally;
     * the normal completion path restores and frees these snapshots earlier. */
    if (t->nommu_vfork_snaps) {
        for (int i = 0; i < t->nommu_num_vfork_snapshots; i++) {
            if (t->nommu_vfork_snaps[i].data)
                kfree(t->nommu_vfork_snaps[i].data);
        }
        kfree(t->nommu_vfork_snaps);
        t->nommu_vfork_snaps = NULL;
    }
    t->nommu_num_vfork_snapshots = 0;
#else
    (void)opaque;
#endif
}

static void release_kernel_stack(void *opaque)
{
    task_t *t = release_task(opaque);
    /* proc_put's last-reference caller runs on another live stack; the task's
     * saved kernel stack is no longer active when this callback runs. */
    if (t->kstack) {
        kfree(t->kstack_base);
        t->kstack = 0;
        t->kstack_base = NULL;
    }
}

static void release_thread_leader(void *opaque)
{
    /* Keep this anchor until all other task resources are gone: tg_prev_ptr
     * may target leader-owned storage while references inspect tg_leader. */
    proc_tg_leader_ref_release(release_task(opaque));
}

static void release_task_object(void *opaque)
{
    a20_task_object_task_release(release_task(opaque));
}

static void release_pid(void *opaque)
{
    proc_pid_final_release(release_task(opaque));
}

static void free_task_storage(void *opaque)
{
    task_t *t = release_task(opaque);
    proc_lifetime_note_task_free();
    memset(t, 0, sizeof(*t));
    kfree(t);
}
#endif

/* Called only after proc_put has atomically claimed the final reference.  The
 * Rust selector chooses this order; each callback below remains one concrete
 * C resource primitive, and the final callback may free t. */
static void proc_task_final_release(task_t *t)
{
    if (!t)
        return;
#ifdef CONFIG_TASK_EXIT_RUST
    const task_release_ops_t ops = {
        .context = t,
        .cgroup = release_cgroup,
        .udriver = release_udriver,
        .process_locks = release_process_locks,
        .files = release_files,
        .mm = release_mm,
        .arch = release_arch,
        .keyring = release_keyring,
        .landlock = release_landlock,
        .envelope = release_envelope,
        .thread_signal_info = release_thread_signal_info,
        .signal_state = release_signal_state,
        .scratch = release_scratch,
        .handle_table = release_handle_table,
        .nommu_vfork = release_nommu_vfork,
        .kernel_stack = release_kernel_stack,
        .thread_leader = release_thread_leader,
        .task_object = release_task_object,
        .pid = release_pid,
        .free_task = free_task_storage,
    };
    a20_task_release_run(&ops);
#else
    if (t->cgroup) {
        cg_detach_task(t);
        t->cgroup = NULL;
    }
    udriver_task_cleanup(t->pid);
    vfs_release_process_locks(t->pid);
    fdtable_close_all(t);
    if (t->mm) {
        mm_destroy(t->mm);
        t->mm = NULL;
    }
    t->pgdir = NULL;
#ifdef ARCH_TASK_RELEASE
    ARCH_TASK_RELEASE(t);
#endif
    keyring_release_task(t);
    landlock_release_task(t);
    env_release_task(t);
    kfree(t->thread_signal_info);
    t->thread_signal_info = NULL;
    if (t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        t->signals = NULL;
        if (refcount_dec_and_test(&ss->refcount))
            kfree(ss);
    }
    void *scratch = __atomic_exchange_n(&t->scratch_buf, NULL, __ATOMIC_ACQ_REL);
    if (scratch) {
        kfree(scratch);
        t->scratch_size = 0;
    }
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    /* Handle tables are distinct from scratch_buf and can be shared across a
     * thread group; never let the Linux scratch cleanup free this object. */
    void *ht = __atomic_exchange_n(&t->a20_ht, NULL, __ATOMIC_ACQ_REL);
    if (ht)
        a20_ht_put_ref((struct a20_ht_internal *)ht);
#endif
#ifdef CONFIG_NOMMU
    if (t->nommu_vfork_snaps) {
        for (int i = 0; i < t->nommu_num_vfork_snapshots; i++)
            if (t->nommu_vfork_snaps[i].data)
                kfree(t->nommu_vfork_snaps[i].data);
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
    /* Keep the leader anchor until all member-owned fields and references are
     * gone: group walkers can still follow the leader's tg_next storage. */
    proc_tg_leader_ref_release(t);
#endif
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

#ifdef A20_TASK_OBJECTS_ENABLED
/* Failure-only snapshot. Keep formatting and extra argument setup out of the
 * normal reference-release path; no locks may be acquired from this helper. */
static __attribute__((cold, noinline, noreturn)) void
proc_ref_underflow(task_t *t, int old) {
    proc_lifetime_note_ref_underflow();
    panic("proc_put: pid=%d reference underflow old=%d task=%p state=%d "
          "destroy=%d dynamic=%d on_rq=%d on_cpu=%d dispatch=%d owner=%u "
          "kstack=%p",
          t->pid, old, t, t->state, t->destroy_started, t->dynamic_alloc,
          t->on_rq, t->on_cpu, t->dispatching, t->owner_cpu, t->kstack_base);
}
#endif

void proc_put(task_t *t)
{
    if (!t)
        return;

#ifdef CONFIG_TASK_EXIT_RUST
    int old = a20_task_ref_dec(&t->refs.value);
#else
    int old = __atomic_load_n(&t->refs.value, __ATOMIC_RELAXED);
    while (old > 0 &&
           !__atomic_compare_exchange_n(&t->refs.value, &old, old - 1, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
    }
#endif
    if (old <= 0) {
#ifdef A20_TASK_OBJECTS_ENABLED
        proc_ref_underflow(t, old);
#else
        proc_lifetime_note_ref_underflow();
        panic("proc_put: pid=%d reference underflow old=%d", t->pid, old);
#endif
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

    proc_task_final_release(t);
#ifdef CONFIG_TASK_EXIT_RUST
    return; /* a20_task_release_run frees the task in its final callback */
#else
    a20_task_object_task_release(t);
    proc_pid_final_release(t);
    proc_lifetime_note_task_free();
    memset(t, 0, sizeof(*t));
    kfree(t);
#endif
}

mm_struct_t *proc_task_get_mm(task_t *t)
{
    if (!t)
        return NULL;
    /* The ->mm pointer is published together with the task's scheduling state,
     * so it is read under the same park_lock. */
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    mm_struct_t *mm = mm_get(t->mm);
    spin_unlock_irqrestore(&t->park_lock, flags);
    return mm;
}

int proc_task_may_access(const task_t *caller, const task_t *target)
{
    if (!caller || !target)
        return 0;
    /* Credentials and the thread-group id are per-task state, so the two-task
     * INV-P3 ordering applies: caller and target are locked in ascending
     * task-pointer order rather than in argument order.  Acquiring a spinlock
     * needs a non-const park_lock, so the const qualification of this
     * predicate's parameters is dropped for the lock/unlock pair only; the
     * body below still reads both tasks and never writes them. */
    task_t *a = (task_t *)caller;
    task_t *b = (task_t *)target;
    uint64_t cf = 0, tf = 0;
    proc_lock_two_tasks(a, b, &cf, &tf);
    int allowed = caller->tgid == target->tgid ||
        userns_capable(caller, userns_task_own((task_t *)target), CAP_SYS_PTRACE) ||
        (mm_dumpable(target->mm) == 1 &&
         caller->cred.fsgid == target->cred.gid &&
         caller->cred.fsgid == target->cred.egid &&
         caller->cred.fsgid == target->cred.sgid &&
         caller->cred.fsuid == target->cred.uid &&
         caller->cred.fsuid == target->cred.euid &&
         caller->cred.fsuid == target->cred.suid);
    proc_unlock_two_tasks(a, b, cf, tf);
    return allowed;
}

void proc_destroy_task(task_t *t)
{
    if (!t)
        return;

    /* D: the UNUSED transition, the timer cancellation and the runqueue
     * removal are one park_lock critical section (park_lock -> timer heap /
     * runq_lock); the list unlink nests tasklist_lock outside it. */
    uint64_t list_flags = spin_lock_irqsave(&tasklist_lock);
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    if (t->destroy_started) {
        proc_lifetime_note_duplicate_destroy();
        spin_unlock_irqrestore(&t->park_lock, flags);
        spin_unlock_irqrestore(&tasklist_lock, list_flags);
        return;
    }
    if (proc_task_lifecycle_advance(t, PROC_TASK_EVENT_DESTROY) < 0) {
        spin_unlock_irqrestore(&t->park_lock, flags);
        spin_unlock_irqrestore(&tasklist_lock, list_flags);
        panic("proc_destroy_task: invalid lifecycle pid=%d phase=%u",
              t->pid, t->lifecycle_phase);
    }
#ifdef A20_TASK_OBJECTS_ENABLED
    __atomic_store_n(&t->destroy_started, 1, __ATOMIC_RELEASE);
#else
    t->destroy_started = 1;
#endif
    t->state = PROC_UNUSED;
    proc_wait_timer_cancel(t, t->wait_seq);
    proc_alarm_cancel(t);
    proc_sched_dl_release(t);
    proc_runq_remove_locked(t);
    spin_unlock_irqrestore(&t->park_lock, flags);
    proc_unlink_task_locked(t);
    spin_unlock_irqrestore(&tasklist_lock, list_flags);

#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
    /* Drop test-only per-task pins if the armed receive mediation was never
     * reached or its result was not collected before task teardown. */
    a20_envelope_race_test_cancel_task(t);
#endif
#ifdef CONFIG_TASK_CONSTRUCTION_TEST
    proc_task_construction_test_cancel(t);
#endif

    a20_task_object_detach(t);
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
