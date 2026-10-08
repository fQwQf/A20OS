#ifndef _PROC_H
#define _PROC_H

#include "core/types.h"
#include "core/consts.h"
#include "core/trap.h"
#include "core/defs.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "proc/park.h"
#include "proc/pidns.h"
#include "proc/userns.h"
#include <signal_abi.h>

struct signal_state;
struct mnt_namespace;
struct mm_struct;
struct mm_seg;
struct files_struct;
struct vmo;
struct cg_node;
typedef struct mm_struct mm_struct_t;

struct vnode;
struct mount;

/*
 * Per-process filesystem position.
 *
 * cwd[] and root_path[] are the flattened spellings the path walker composes
 * against; the vnode/mount fields next to them are the authority.  A process
 * holds a reference on the directory object its cwd names and on the (mount,
 * vnode) pair that is its root, which is what makes chroot(2) and
 * pivot_root(2) operate on objects rather than on string prefixes:
 *
 *   - unmounting the filesystem a process is rooted in, or standing in, is
 *     refused because the mount can see those references;
 *   - pivot_root can therefore detach the old root without leaving any
 *     process holding a mount that nothing reaches any more;
 *   - a fork copies the pointers and takes its own references, so the parent
 *     and child can chdir/pivot independently.
 *
 * fdtable_close_all() releases both references; that is the single teardown
 * hook every exit path already calls.
 */
typedef struct proc_fs_context {
    char cwd[MAX_PATH_LEN];
    char root_path[MAX_PATH_LEN];
    struct vnode *cwd_vn;      /* pin on the directory the cwd names */
    struct mount *root_mnt;    /* mount the process root lives in */
    struct vnode *root_vn;     /* the root directory object itself */
    int  umask;
} proc_fs_context_t;

typedef struct proc_cred {
    int      uid;
    int      euid;
    int      suid;
    int      fsuid;
    int      gid;
    int      egid;
    int      sgid;
    int      fsgid;
    int      ngroups;
    int      groups[MAX_GROUPS];
    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t cap_bounding;
} proc_cred_t;

#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_SYS_MODULE       16
#define CAP_SYS_CHROOT       18
#define CAP_SYS_PTRACE       19
#define CAP_SYS_NICE         23
#define CAP_SYS_ADMIN        21
#define CAP_SYS_RESOURCE     24
#define CAP_SYS_BOOT         22
#define CAP_NET_RAW          13

typedef struct proc_limits {
    uint64_t stack;
    uint64_t nofile;
    uint64_t memlock;
    uint64_t as;      /* RLIMIT_AS: address-space bytes, 0 = unlimited */
    uint64_t nproc;   /* RLIMIT_NPROC: processes per uid, 0 = unlimited */
} proc_limits_t;

typedef struct proc_policy {
    int oom_score_adj;
    int thp_disabled;
    int mempolicy_mode;
    uint64_t mempolicy_nmask;
} proc_policy_t;

typedef struct proc_ns_context {
    char     fs_root[MAX_PATH_LEN];
    uint32_t net_ifindex;
    uint64_t pid_offset;
    uint32_t dev_access_mask;
    uint32_t active_ns;
} proc_ns_context_t;

typedef struct proc_vm_stats {
    size_t anon_huge_pages;
    size_t shmem_huge_pages;
    size_t file_huge_pages;
} proc_vm_stats_t;

/*
 * task_t lifetime and state invariants:
 * - The idle task is static; normal tasks are dynamically allocated, linked
 *   into the global task list, and released only after they are unreachable
 *   from PID lookup, parent/wait lists, run queues, wait/timer entries, and
 *   current CPU slots. Every pointer which survives its protecting lock owns a
 *   task reference.
 * - Normal task state flows are:
 *     PROC_UNUSED -> PROC_READY -> PROC_RUNNING
 *     PROC_RUNNING -> PROC_READY     (yield/preemption)
 *     PROC_RUNNING -> PROC_BLOCKED   (wait queue, sleep, child wait, futex)
 *     PROC_BLOCKED -> PROC_READY     (wake, timeout, signal)
 *     PROC_RUNNING -> PROC_STOPPED   (job-control stop)
 *     PROC_STOPPED -> PROC_READY     (SIGCONT, fatal signal, or task exit)
 *     PROC_RUNNING/BLOCKED -> PROC_ZOMBIE -> PROC_UNUSED
 *   A task must not be put on a run queue unless its state is PROC_READY, and a
 *   zombie or unused task must never be requeued.
 * - Lock ownership (design lock-serialization-split §1.2):
 *     task->park_lock  owns ->state / ->park_state / ->wait_seq / ->wake_reason /
 *                      ->on_cpu, plus the ptrace and ->mm / ->files / ->cred
 *                      fields that travel with the task;
 *     per-CPU runqueue lock owns ->on_rq / ->cpu_id / ->sched_level /
 *                      ->ready_since and publishes ->dispatching / ->owner_cpu
 *                      (atomic reads elsewhere);
 *     tasklist_lock    owns task-list membership (->all_next/->all_prev) and
 *                      the parent/children and thread-group chains.  It does
 *                      NOT protect scheduling state.
 *   PID lookup has its own pid_lock and signal pending/actions have
 *   signal_state.lock.  Local pick atomically publishes on_rq -> dispatching
 *   under only that CPU's runqueue lock and then releases it before switch
 *   publication takes the selected task's park_lock.  Every path needing two
 *   of these locks follows tasklist_lock -> park_lock -> runq_lock; a path
 *   needing two task park_locks takes them in ascending task-pointer order
 *   (proc_lock_two_tasks).
 * - cpu_id selects the owning run queue while on_rq is true. Code that changes
 *   cpu_id for a queued task must first remove it from its current run queue or
 *   hold the locks needed to move it atomically.
 * - on_rq, dispatching, and on_cpu are mutually exclusive. owner_cpu identifies
 *   the CPU which selected or still owns a dispatching/on_cpu task; it is
 *   PROC_CPU_NONE otherwise.
 * - proc_current()/proc_set_current() use CPU-local slots. A task remains
 *   on_cpu until the replacement task has taken over the kernel stack and
 *   proc_switch_complete() releases the old ownership.
 * - External modules should prefer proc_* and signal_* helpers instead of
 *   directly changing state, credentials, fs context, or run-queue fields.
 *
 * TASK_STATE_MUTATION_CONTRACT:
 * - New-task activation goes through proc_make_ready(). STOPPED tasks resume
 *   only through proc_sched_resume_stopped() for SIGCONT, fatal signal, or
 *   task exit. A parked task is resumed only by proc_try_wake() with the
 *   matching wait token and a reason allowed by its wait mode.
 * - Timed or indefinite sleeps go through the Park/Wake protocol or a wait
 *   object. The caller registers object-specific waiter state before commit.
 * - RUNNING is assigned only by context_switch()/sched() after a task has moved
 *   from on_rq to dispatching. A READY task that is still on_cpu is queued only
 *   by proc_switch_complete(). ZOMBIE/UNUSED are exit/reap states and must not
 *   be written by synchronization primitives.
 */
#define PROC_CPU_NONE ((unsigned)-1)

#ifdef CONFIG_NOMMU
typedef struct nommu_vfork_snap_entry {
    void   *dst;
    void   *data;
    size_t  size;
} nommu_vfork_snap_entry_t;
#endif

/*
 * Intrusive EEVDF runqueue node (kernel/proc/sched.c owns the tree code).
 * The per-CPU runqueue keeps normal tasks in a randomized (treap) BST ordered
 * by earliest virtual deadline and augmented with the subtree-minimum
 * vruntime so the eligible-gated pick can prune ineligible subtrees in
 * O(log n) instead of walking a sorted list.
 */
typedef struct eevdf_node {
    struct eevdf_node *left;
    struct eevdf_node *right;
    struct eevdf_node *parent;
    uint64_t min_vruntime;   /* min eevdf_vruntime within this subtree */
    uint32_t heap_prio;      /* treap heap key (smaller = closer to root) */
} eevdf_node_t;

typedef struct task_t {
    /*
     * Architecture context-switch assembly depends on these two offsets.
     * Keep them first and guard the layout with static assertions below.
     */
    uintptr_t kstack;
    void    *kstack_base;
    /* The kernel-mode sp guard in trap.S (riscv64) runs before the register
     * frame is saved and must not clobber any live register, so its scratch
     * slots live in the task struct and are addressed through tp, which in
     * kernel mode always holds the current task_t (see switch.S).  The guard
     * reaches them at fixed offsets. */
    uintptr_t trap_guard_scratch[7];
#ifdef CONFIG_KSTACK_DIAG
    /* Temporary allocation identity for the CI-only stack corruption probe. */
    void     *kstack_diag_base;
    uint32_t  kstack_diag_seq;
    uint8_t   kstack_diag_reported;
#endif
    refcount_t refs;
    int      destroy_started;
    int      pid;
    int      tgid;
    int      ppid;
    proc_state_t state;
    vaddr_t  ustack;
    pt_root_t *pgdir;
    trap_context_t *trap_ctx;
    int      exit_code;
    struct files_struct *files;
    proc_fs_context_t fs;
    int      vfs_open_errno;   /* specific error from the last failed vnode open */
    int      lookup_errno;     /* error from the last failed path resolution
                                (was the global g_lookup_errno, which made
                                concurrent lookups overwrite each other's
                                failure reason) */
    struct task_t *parent;
    /* Parent-children membership, kept in lockstep with ->parent under
     * tasklist_lock so wait4/reparent walk O(children) instead of the global
     * task list. */
    struct task_t *sibling_next;
    struct task_t **sibling_prev_ptr;
    struct task_t *children;
    /* Thread-group chain rooted at the leader (leader excluded from its own
     * tg_next chain); tg_leader is self for non-thread tasks. */
    struct task_t *tg_leader;
    struct task_t *tg_next;
    struct task_t **tg_prev_ptr;
    uint64_t wake_time;
    uint64_t alarm_expire;
    uint64_t itimer_real_interval;
    uint64_t itimer_values[3][4];
    int      priority;
    int      sched_level;
    unsigned cpu_id;
    int      on_rq;
    int      dispatching;
    int      on_cpu;
    unsigned owner_cpu;
    int      vfork_waiting;
#ifdef CONFIG_NOMMU
    /* A NOMMU vfork child shares writable memory until exec/exit. */
    nommu_vfork_snap_entry_t *nommu_vfork_snaps;
    int nommu_num_vfork_snapshots;
#endif
    struct task_t *rq_next;
    struct task_t *rq_prev;
    eevdf_node_t eevdf_node;  /* EEVDF runqueue treap membership (runq lock) */
    struct task_t *wait_next;
    uint64_t total_time;
    uint64_t utime_ticks;          /* scheduler ticks charged in user mode */
    uint64_t stime_ticks;          /* scheduler ticks charged in kernel mode */
    uint64_t start_jiffies;        /* creation time, 10 ms jiffies since boot */
    uint64_t child_utime;
    uint64_t child_stime;
    uint64_t exec_start;
    uint64_t ready_since;
    uint64_t perf_page_faults;     /* perf_event_open PERF_COUNT_SW_PAGE_FAULTS */
    uint64_t perf_page_faults_maj; /* faults that performed backing I/O */
    uint64_t perf_switches;        /* PERF_COUNT_SW_CONTEXT_SWITCHES */
    /* Context switches by cause, for getrusage's ru_nvcsw/ru_nivcsw.  A task
     * that already moved itself to PROC_BLOCKED yielded voluntarily; anything
     * else was preempted.  perf_switches is the sum of the two. */
    uint64_t perf_switches_vol;
    uint64_t perf_switches_invol;
    /* /proc/<pid>/io accounting.  rchar/wchar are bytes through read/write(2);
     * read_bytes/write_bytes are bytes actually moved to/from the block
     * device, so cached I/O leaves them near zero.  Relaxed: these are
     * statistics and must not order the I/O they describe. */
    uint64_t io_rchar;
    uint64_t io_wchar;
    uint64_t io_syscr;
    uint64_t io_syscw;
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t user_gs_base;         /* x86_64 ARCH_SET_GS value (kernel GS is
                                    * reserved for per-CPU data) */
    uint32_t cfs_weight;
    uint32_t pi_boost_weight;    /* PI-futex waiter weight donation, atomic */
    uint64_t eevdf_vruntime;     /* weighted virtual run time (ticks) */
    uint64_t eevdf_deadline;     /* virtual deadline: vruntime + virtual slice */
    uint64_t eevdf_last_account; /* tick stamp of the last vruntime charge */
    int      sched_policy;
    int      sched_reset_on_fork;
    int      waiting_for_child;
    int      exit_pending;
    int      pending_exit_code;
    int      stop_report_pending;
    int      continue_report_pending;

    struct signal_state *signals;

    mm_struct_t *mm;

    uintptr_t entry;
    uintptr_t first_kernel_entry;
    vaddr_t   exec_load_addr;
    size_t    exec_load_size;

    int       pgid;
    int       sid;

    proc_limits_t limits;
    proc_cred_t   cred;
    proc_policy_t policy;
    int       clone_flags;
    int       exit_signal;
    int       pdeathsig;       /* PR_SET_PDEATHSIG: one-shot signal delivered
                                * when the parent exits (0 = none) */
    int      *clear_child_tid;
    uintptr_t robust_list_head;

    char      name[64];
    char      exec_path[MAX_PATH_LEN];
    struct task_t *pid_hash_next;
    struct task_t *all_next;
    struct task_t *all_prev;
    int       dynamic_alloc;
    void     *scratch_buf;
    size_t    scratch_size;
    void     *a20_ht;   /* Native ABI handle table (separate from scratch_buf,
                         * which is reused by Linux ABI I/O buffers) */
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    /* Native ABI standard handles (fd 0/1/2 + root/cwd/self) declared by the
     * libc via A20_SYS_task_adopt.  exec preserves them for native processes
     * so fork+exec keeps the caller's stdio (e.g. a pipeline read end). */
    uint32_t a20_stdin_h;
    uint32_t a20_stdout_h;
    uint32_t a20_stderr_h;
    uint32_t a20_root_h;
    uint32_t a20_cwd_h;
    uint32_t a20_self_h;
    int      a20_stdio_adopted;
#endif

    trap_context_t sig_saved_ctx;
    uint64_t       sig_blocked;
    uint64_t       sig_old_blocked;
    int            sig_handling;
    uint64_t       sigsuspend_old_blocked;
    int            sigsuspend_active;
    uint64_t       sigwait_mask;
    int            sigwait_active;
    arch_sigaltstack_t sigaltstack;
    uint64_t       thread_pending;

    /* Debug/tracing state (kernel/proc/debug.c).  Protected by the task's own
     * park_lock for transitions; the tracer reads the snapshot fields only
     * while the tracee is in a ptrace stop (ptrace_stop_active), which the
     * tracee publishes under its park_lock before blocking. */
    struct task_t *ptracer;          /* observing task, or NULL */
    int            ptrace_orig_parent_pid; /* real parent pid, restored on detach */
    uint32_t       ptrace_flags;     /* PT_DEBUG_FLAG_* */
    int            ptrace_stop_active;
    int            ptrace_stop_kind; /* PT_DEBUG_STOP_* */
    int            ptrace_event;     /* PT_DEBUG_EVENT_*, 0 = none */
    uint64_t       ptrace_event_msg;
    int            ptrace_stop_sig;  /* signal reported by this stop */
    uint8_t        ptrace_siginfo[128]; /* siginfo snapshot of this stop */
    int            ptrace_siginfo_valid;
    int            ptrace_deliver_sig; /* one-shot no-re-stop signal */
    int            ptrace_ctx_valid;
    int            ptrace_exit_reported; /* zombie exit reported to an observer */
    trap_context_t ptrace_saved_ctx; /* regs snapshot at stop time */

    ARCH_TASK_FIELDS

    /* Native ABI support */
    uint32_t       abi_mode;        /* 0 = Linux ABI, 1 = Native ABI */
    struct vmo *stack_vmo;
    struct vmo *heap_vmo;
    /* Mount namespace membership (kernel/fs/vfs/mntns.c).  NULL means the
     * initial namespace, which is statically pinned and needs no reference.
     * A non-NULL pointer owns one mnt_namespace reference, taken at
     * fork/unshare/setns and released by mntns_release_task() from the
     * per-task teardown in fdtable_close_all(). */
    struct mnt_namespace *mnt_ns;
    /* User namespace membership (kernel/proc/userns.c).  Credentials in
     * proc_cred_t are stored as GLOBAL ids and translated through this
     * namespace's uid_map/gid_map at the syscall and procfs boundary; NULL
     * means the initial namespace, which maps ids to themselves.  Same
     * ownership rule as mnt_ns: one reference, released by
     * userns_release_task() from fdtable_close_all(). */
    struct user_namespace *user_ns;
    proc_ns_context_t ns_ctx;

    /* PID namespace membership (kernel/proc/pidns.c).  pid_ns is the
     * namespace this task is a member of -- its deepest one -- and is what
     * its own ids are reported in; pid_ns_for_children is the namespace a
     * fork() places the next child in.  The two differ exactly between
     * unshare(CLONE_NEWPID) and the next fork.  Each non-NULL pointer owns
     * one pid_namespace reference; NULL means the initial namespace, which is
     * statically pinned. */
    struct pid_namespace *pid_ns;
    struct pid_namespace *pid_ns_for_children;
    /* One id per namespace LEVEL the task is visible in.  Level 0 is the
     * initial namespace and is task_t::pid itself; entries 1..pid_ns_level
     * are the container-local ids. */
    int          ns_pid[PID_MAX_LEVELS];
    int          pid_ns_level;

    /* Kernel keyring subsystem (kernel/ipc/keyring.c).  Owning reference to a
     * keyring object, shared with children at fork and released at teardown. */
    void           *session_keyring;

    /* Seccomp state (kernel/ipc/seccomp.c): SECCOMP_MODE_* plus a refcounted
     * newest-first classic-BPF filter chain shared with children at fork. */
    void           *seccomp_chain;
    uint32_t        seccomp_mode;

    /* Landlock LSM ruleset list (kernel/ipc/landlock.c), process-local. */
    void           *landlock_rulesets;

    /* Capability envelope (kernel/abi/linux/envelope.c): budgeted-capability
     * policy attached to this Linux-ABI process by its supervisor.  NULL =
     * unenveloped (the mediation fast path).  Survives fork (shared refcount)
     * and execve; a process cannot shed it.  See docs/research/05. */
    void           *envelope;

    /* Restartable sequences (rseq(2)): user-registered per-thread rseq area
     * and signature.  0 when not registered. */
    uintptr_t       rseq_area;
    uint32_t        rseq_sig;
    uint32_t        rseq_flags;

    /* Syscall-restart block (SYS_restart_syscall): nr+args recorded by the
     * dispatcher on the -ERESTARTSYS accepted-restart rewind, replayed by
     * SYS_restart_syscall, invalidated when any other syscall dispatches
     * first.  Not inherited across fork. */
    int             restart_active;
    uint64_t        restart_nr;
    uint64_t        restart_args[6];

    /* ioprio_get/ioprio_set(2): the encoded I/O priority (class<<13 | data). */
    int             ioprio;
    /* pkey_alloc/pkey_mprotect(2): allocated protection keys bitmap. */
    uint32_t        pkey_bitset;
#ifdef CONFIG_XLATOR
    /* Set when execve re-execed this task through a foreign-architecture
     * translator (kernel/proc/exec.c).  A qemu-user style translator has
     * to JIT guest code into an RWX buffer, so this is the one task
     * allowed past the user W^X gate in mm_wx_filter_prot().  Only the
     * kernel sets it, on the re-exec path, and it is deliberately not
     * inherited across fork: a child that is itself foreign has to go
     * through execve and the same check again.
     *
     * Conditional on CONFIG_XLATOR (like the NOMMU fields above) so that a
     * build with no translator channel does not carry the byte. */
    uint8_t         xlator_host;
#endif

    /* Cgroup resource control */
    struct cg_node *cgroup;
    uint32_t        cpus_allowed;
    int             cg_throttled;
    uint64_t        cg_cpu_start;

    completion_t vfork_done;

    /* Scheduler-private A20 park/wake state.  park_lock serializes the
     * transitions between PREPARING/PARKED/WOKEN and IDLE plus the timer
     * register/cancel (park_lock -> timer_heap lock) and the runqueue
     * enqueue in the wake path (park_lock -> runq lock).  It must never be
     * held while acquiring tasklist_lock, and never held while acquiring
     * another task's park_lock except through proc_lock_two_tasks(), which
     * imposes the ascending task-pointer order. */
    spinlock_t         park_lock;
    uint64_t           wait_seq;
    uint64_t           wait_deadline;
    int                wait_timer_index;
    int                alarm_timer_index;
    proc_park_state_t  park_state;
    proc_wait_mode_t   wait_mode;
    proc_wake_reason_t wake_reason;
} task_t;

_Static_assert(offsetof(task_t, kstack) == 0,
               "task_t.kstack must remain at assembly ABI offset 0");
_Static_assert(offsetof(task_t, kstack_base) == sizeof(uintptr_t),
               "task_t.kstack_base must remain at assembly ABI offset 8/4");
_Static_assert(offsetof(task_t, trap_guard_scratch) == 2 * sizeof(uintptr_t),
               "task_t.trap_guard_scratch must remain at assembly ABI offset 16/8");

#ifdef CONFIG_KSTACK_DIAG
void proc_kstack_diag_register(task_t *t);
void proc_kstack_diag_check(task_t *t, const char *where);
#endif

#define PROC_SCHED_POLICY   (1U << 0)
#define PROC_SCHED_PRIORITY (1U << 1)
#define PROC_SCHED_AFFINITY (1U << 2)
#define PROC_SCHED_NICE     (1U << 3)

typedef struct proc_sched_config {
    uint32_t fields;
    int policy;
    int priority;
    int nice;
    uint32_t affinity;
    int reset_on_fork;
} proc_sched_config_t;

static inline int proc_has_cap(const task_t *t, int cap)
{
    if (!t) return 1;
    if (cap < 0 || cap >= 64) return 0;
    return (t->cred.cap_effective & (1ULL << cap)) != 0;
}

/* Charge the calling task for one read/write of `bytes` transferred.
 * `to_device` selects the block-layer counter, which is only meaningful
 * once a request actually reaches the device. */
void proc_io_account(uint64_t rbytes, uint64_t wbytes,
                     uint64_t read_to_device, uint64_t write_to_device);

/* /proc/loadavg sampling.  Call proc_loadavg_tick() from the scheduler tick;
 * proc_loadavg_snapshot() reports the EMAs in Q16 fixed point plus the
 * instantaneous running/total task counts. */
void proc_loadavg_tick(void);
void proc_loadavg_snapshot(uint64_t *avg1, uint64_t *avg5, uint64_t *avg15,
                           unsigned *running, unsigned *total, int *max_pid);

/* ---- Process management API ---- */
void     proc_init(void);
void     proc_init_secondary(unsigned cpu_id);
void     idle_loop(void) NORETURN;
task_t  *proc_current(void);
void     proc_sleep_until(uint64_t wake_time);
/*
 * SMP_RUNQUEUE_PREEMPT_PROTOCOL:
 * Reschedule requests are persistent per-CPU state. Architecture IPI handlers
 * only acknowledge notification; scheduling is consumed by a common trap or
 * syscall return safe point, a timer-return safe point, or an explicit sched().
 */
void     proc_sched_handle_reschedule_ipi(void);
void     proc_sched_tick(int from_user);
void     proc_sched_pi_boost(task_t *owner, task_t *pi_waiter);
void     proc_sched_pi_unboost(task_t *owner);
uint64_t proc_runq_load_sum(void);
void     proc_get_cpu_times(unsigned cpu, uint64_t *user, uint64_t *system,
                            uint64_t *idle);
void     proc_sched_request_current(void);
int      proc_sched_safe_point(void);
/* Called by the trap layer at the IRQ return point; yields the current task
 * when this CPU is preemptible and a reschedule is pending.  Never called from
 * a synchronous-exception path -- see kernel/core/trap.c. */
void     kernel_preempt_at_irq_return(void);
/*
 * TASK_REFERENCE_LIFETIME:
 * proc_find_get() returns a referenced task which remains valid after the PID
 * lock is released. Every successful lookup must be paired with proc_put().
 * proc_get() is for scheduler/wait/timer owners which already have a live task.
 */
task_t  *proc_get(task_t *task);
void     proc_put(task_t *task);
task_t  *proc_find_get(int pid);
mm_struct_t *proc_task_get_mm(task_t *task);
int proc_task_may_access(const task_t *caller, const task_t *target);
int      proc_pid_max(void);
int      proc_set_pid_max(int value);
void     proc_get_vm_stats(proc_vm_stats_t *stats);
size_t   proc_format_pidmap(char *buf, size_t bufsz);
int      proc_alloc(void (*entry)(void));
int      proc_alloc_user(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir);
int      proc_alloc_user_image(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir,
                               struct mm_seg *mmap, vaddr_t brk,
                               vaddr_t stack_top, size_t total_vm,
                               vaddr_t tls_tp
#ifdef CONFIG_NOMMU
                          , void **nommu_allocs, const size_t *nommu_alloc_sizes,
                           const uint8_t *nommu_alloc_types, int num_nommu_allocs
#endif
                          , int defer_ready);
/* Publish a fully initialized task created with defer_ready=1. */
void     proc_publish_deferred_task(task_t *task);
void     proc_free_pid(int pid);
void     proc_exit(int exit_code) NORETURN;
void     proc_exit_group(int exit_code) NORETURN;
void     proc_exec_terminate_siblings(task_t *self);
void     proc_force_exit(task_t *t, int exit_code);
void     proc_check_exit_pending(void);
int      proc_wait4(int pid, int *status, int options);
void     proc_yield(void);
int      proc_sched_get(task_t *t, proc_sched_config_t *out);
int      proc_sched_set(task_t *t, const proc_sched_config_t *config);
int      proc_sched_priority_range(int policy, int *min, int *max);
uint32_t proc_sched_effective_affinity(task_t *t);
void     sched(void);
void     context_switch(task_t *next);
uint64_t proc_next_timer_interval(uint64_t now);
void     proc_set_alarm_expire(task_t *t, uint64_t alarm_expire);
void     sched_note_timer_deadline(uint64_t deadline);
void     sched_set_posix_deadline(uint64_t deadline);
void     proc_dump(void);
int      proc_kill(int pid, int signum);
int      proc_kill_pgid(int pgid, int signum, int skip_self);
int      proc_pgid_alive(int pgid);
void     proc_set_name(task_t *t, const char *name);
void     proc_make_ready(task_t *t);
void    *proc_scratch_buffer(size_t size);

/* For execve: replace current process image */
int      proc_exec(const char *path, char *const argv[], char *const envp[]);

/* mmap/brk helpers */
vaddr_t  proc_brk(vaddr_t newbrk);
struct vfile;
vaddr_t  proc_mmap(vaddr_t addr, size_t len, int prot, int flags, int fd, long off);
/* Variant for callers already holding a vfile reference (consumed). */
vaddr_t  proc_mmap_vfile(vaddr_t addr, size_t len, int prot, int flags,
                         struct vfile *file, long off);
int      proc_munmap(vaddr_t addr, size_t len);

/* Clone (fork-like) */
int      proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid, int exit_signal);

/* Native ABI thread creation: shares address space / fd table / handle table,
 * starts at `entry` with `arg` in the first argument register. */
int      proc_create_thread(uint64_t entry, uint64_t arg, vaddr_t sp, vaddr_t tls);

/* Global task-list membership (task_list_head/tail, ->all_next/->all_prev) and
 * the parent/children/sibling plus thread-group chains are guarded by
 * tasklist_lock, which does NOT protect scheduling state. */
task_t *proc_first_task_locked(void);
task_t *proc_next_task_locked(task_t *t);

/*
 * Consistent read of a task's scheduling state.
 *
 * ->state and ->on_cpu are park_lock-owned (INV-P1), while ->on_rq/->cpu_id are
 * runq_lock-owned and ->dispatching/->owner_cpu are published under the
 * runqueue lock (INV-P2/INV-P4b).  A low-frequency reader that walks the task
 * list under tasklist_lock must NOT read those fields directly: doing so has no
 * synchronisation relationship with any writer.  Take a snapshot instead.
 */
typedef struct proc_task_sched_state {
    proc_state_t state;
    int          on_cpu;
    int          task_on_rq;
    int          dispatching;
    unsigned     owner_cpu;
    unsigned     task_cpu_id;
} proc_task_sched_state_t;

void proc_task_sched_state_snapshot(task_t *t, proc_task_sched_state_t *out);
/* Convenience wrapper for callers that only need ->state. */
int  proc_task_state_get(task_t *t);

#endif /* _PROC_H */
