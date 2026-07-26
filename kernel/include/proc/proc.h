#ifndef _PROC_H
#define _PROC_H

#include "core/types.h"
#include "core/consts.h"
#include "core/trap.h"
#include "core/defs.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "proc/park.h"
#include <signal_abi.h>

struct signal_state;
struct mm_struct;
struct vm_area;
struct files_struct;
struct a20_vmo;
struct cg_node;
typedef struct mm_struct mm_struct_t;

typedef struct proc_fs_context {
    char cwd[MAX_PATH_LEN];
    char root_path[MAX_PATH_LEN];
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
#define CAP_SYS_CHROOT       18
#define CAP_SYS_ADMIN        21
#define CAP_SYS_RESOURCE     24
#define CAP_NET_RAW          13

typedef struct proc_limits {
    uint64_t stack;
    uint64_t nofile;
    uint64_t memlock;
} proc_limits_t;

typedef struct proc_policy {
    int oom_score_adj;
    int thp_disabled;
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
 *     PROC_RUNNING/BLOCKED -> PROC_ZOMBIE -> PROC_UNUSED
 *   A task must not be put on a run queue unless its state is PROC_READY, and a
 *   zombie or unused task must never be requeued.
 * - proc_lock protects allocation, PID membership, all-task list membership,
 *   parent/wait relationships, zombie/reap transitions, and most task state
 *   transitions and scheduler ownership metadata. Per-CPU runqueue locks
 *   protect rq_next/rq_prev/on_rq and queue membership; callers that need both
 *   follow proc_lock -> runq_lock.
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
 * - New-task activation and STOPPED-task resumption go through
 *   proc_make_ready(). A parked task is resumed only by proc_try_wake() with
 *   the matching wait token.
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

typedef struct task_t {
    /*
     * Architecture context-switch assembly depends on these two offsets.
     * Keep them first and guard the layout with static assertions below.
     */
    uintptr_t kstack;
    void    *kstack_base;
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
    struct task_t *parent;
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
    struct task_t *wait_next;
    uint64_t total_time;
    uint64_t child_utime;
    uint64_t child_stime;
    uint64_t exec_start;
    uint64_t ready_since;
    uint32_t cfs_weight;
    int      sched_policy;
    int      sched_reset_on_fork;
    int      waiting_for_child;
    int      exit_pending;
    int      pending_exit_code;

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

    trap_context_t sig_saved_ctx;
    uint64_t       sig_blocked;
    uint64_t       sig_old_blocked;
    int            sig_handling;
    uint64_t       sigsuspend_old_blocked;
    int            sigsuspend_active;
    arch_sigaltstack_t sigaltstack;
    uint64_t       thread_pending;

    ARCH_TASK_FIELDS

    /* Native ABI support */
    uint32_t       abi_mode;        /* 0 = Linux ABI, 1 = Native ABI */
    struct a20_vmo *stack_vmo;
    struct a20_vmo *heap_vmo;
    proc_ns_context_t ns_ctx;

    /* Cgroup resource control */
    struct cg_node *cgroup;
    uint32_t        cpus_allowed;
    int             cg_throttled;
    uint64_t        cg_cpu_start;

    completion_t vfork_done;

    /* Scheduler-private A20 park/wake state. */
    uint64_t           wait_seq;
    uint64_t           wait_deadline;
    int                wait_timer_index;
    proc_park_state_t  park_state;
    proc_wait_mode_t   wait_mode;
    proc_wake_reason_t wake_reason;
} task_t;

_Static_assert(offsetof(task_t, kstack) == 0,
               "task_t.kstack must remain at assembly ABI offset 0");
_Static_assert(offsetof(task_t, kstack_base) == sizeof(uintptr_t),
               "task_t.kstack_base must remain at assembly ABI offset 8/4");

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

/* ---- Process management API ---- */
void     proc_init(void);
void     proc_init_secondary(unsigned cpu_id);
void     idle_loop(void) NORETURN;
task_t  *proc_current(void);
void     proc_sleep_until(uint64_t wake_time);
/*
 * TASK_REFERENCE_LIFETIME:
 * proc_find_get() returns a referenced task which remains valid after the PID
 * lock is released. Every successful lookup must be paired with proc_put().
 * proc_get() is for scheduler/wait/timer owners which already have a live task.
 */
task_t  *proc_get(task_t *task);
void     proc_put(task_t *task);
task_t  *proc_find_get(int pid);
int      proc_pid_max(void);
int      proc_set_pid_max(int value);
void     proc_get_vm_stats(proc_vm_stats_t *stats);
size_t   proc_format_pidmap(char *buf, size_t bufsz);
int      proc_alloc(void (*entry)(void));
int      proc_alloc_user(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir);
int      proc_alloc_user_image(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir,
                               struct vm_area *mmap, vaddr_t brk,
                               vaddr_t stack_top, size_t total_vm,
                               vaddr_t tls_tp
#ifdef CONFIG_NOMMU
                          , void **nommu_allocs, const size_t *nommu_alloc_sizes,
                          const uint8_t *nommu_alloc_types, int num_nommu_allocs
#endif
                          );
void     proc_free_pid(int pid);
void     proc_exit(int exit_code) NORETURN;
void     proc_exit_group(int exit_code) NORETURN;
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
void     proc_dump(void);
int      proc_kill(int pid, int signum);
int      proc_kill_pgid(int pgid, int signum, int skip_self);
void     proc_set_name(task_t *t, const char *name);
void     proc_make_ready(task_t *t);
void    *proc_scratch_buffer(size_t size);

/* For execve: replace current process image */
int      proc_exec(const char *path, char *const argv[], char *const envp[]);

/* mmap/brk helpers */
vaddr_t  proc_brk(vaddr_t newbrk);
vaddr_t  proc_mmap(vaddr_t addr, size_t len, int prot, int flags, int fd, long off);
int      proc_munmap(vaddr_t addr, size_t len);

/* Clone (fork-like) */
int      proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid, int exit_signal);

task_t *proc_first_task_locked(void);
task_t *proc_next_task_locked(task_t *t);

#endif /* _PROC_H */
