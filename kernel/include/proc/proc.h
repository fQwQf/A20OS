#ifndef _PROC_H
#define _PROC_H

#include "core/types.h"
#include "core/consts.h"
#include "core/trap.h"
#include "core/defs.h"

struct signal_state;
struct mm_struct;
struct vm_area;
struct files_struct;
typedef struct mm_struct mm_struct_t;

typedef struct { void *ss_sp; int ss_flags; size_t ss_size; } sigaltstack_t;

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

typedef struct proc_limits {
    uint64_t stack;
    uint64_t nofile;
} proc_limits_t;

typedef struct proc_policy {
    int oom_score_adj;
    int thp_disabled;
} proc_policy_t;

typedef struct proc_vm_stats {
    size_t anon_huge_pages;
    size_t shmem_huge_pages;
    size_t file_huge_pages;
} proc_vm_stats_t;

/*
 * task_t lifetime:
 * - The idle task is static; normal tasks are dynamically allocated and linked
 *   into the global task list.
 * - proc_lock protects allocation, PID/run-queue membership, and most state
 *   transitions.
 * - proc_current()/proc_set_current() use CPU-local slots. SMP still needs
 *   scheduler and locking work before tasks can run concurrently on CPUs.
 * - External modules should prefer proc_* and signal_* helpers instead of
 *   directly changing state, credentials, fs context, or run-queue fields.
 */
typedef struct task_t {
    uint64_t kstack;
    void    *kstack_base;
    int      pid;
    int      tgid;
    int      ppid;
    proc_state_t state;
    vaddr_t  ustack;
    uint64_t *pgdir;
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
    struct task_t *rq_next;
    struct task_t *rq_prev;
    struct task_t *wait_next;
    uint64_t total_time;
    uint64_t child_utime;
    uint64_t child_stime;

    struct signal_state *signals;

    mm_struct_t *mm;

    uint64_t  entry;
    uint64_t  exec_load_addr;
    size_t    exec_load_size;

    int       pgid;
    int       sid;

    proc_limits_t limits;
    proc_cred_t   cred;
    proc_policy_t policy;
    int       clone_flags;
    int      *clear_child_tid;

    char      name[64];
    char      exec_path[MAX_PATH_LEN];
    struct task_t *pid_hash_next;
    struct task_t *all_next;
    struct task_t *all_prev;
    int       dynamic_alloc;
    void     *scratch_buf;
    size_t    scratch_size;

    trap_context_t sig_saved_ctx;
    uint64_t       sig_old_blocked;
    int            sig_handling;
    uint64_t       sigsuspend_old_blocked;
    int            sigsuspend_active;
    sigaltstack_t  sigaltstack;
} task_t;

static inline int proc_has_cap(const task_t *t, int cap)
{
    if (!t) return 1;
    if (cap < 0 || cap >= 64) return 0;
    return (t->cred.cap_effective & (1ULL << cap)) != 0;
}

/* ---- Process management API ---- */
void     proc_init(void);
void     idle_loop(void) NORETURN;
task_t  *proc_current(void);
task_t  *proc_find(int pid);
int      proc_pid_max(void);
int      proc_set_pid_max(int value);
void     proc_get_vm_stats(proc_vm_stats_t *stats);
size_t   proc_format_pidmap(char *buf, size_t bufsz);
int      proc_alloc(void (*entry)(void));
int      proc_alloc_user(uint64_t entry, uint64_t sp, uint64_t *pgdir);
int      proc_alloc_user_image(uint64_t entry, uint64_t sp, uint64_t *pgdir,
                               struct vm_area *mmap, uint64_t brk,
                               uint64_t stack_top, size_t total_vm);
void     proc_free_pid(int pid);
void     proc_exit(int exit_code) NORETURN;
void     proc_exit_group(int exit_code) NORETURN;
void     proc_force_exit(task_t *t, int exit_code);
int      proc_wait4(int pid, int *status, int options);
void     proc_yield(void);
void     sched(void);
void     context_switch(task_t *next);
void     proc_set_wake_time(task_t *t, uint64_t wake_time);
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
uint64_t proc_brk(uint64_t newbrk);
uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off);
int      proc_munmap(uint64_t addr, size_t len);

/* Clone (fork-like) */
int      proc_clone(uint64_t flags, uint64_t stack, int *ptid, uint64_t tls, int *ctid);

#endif /* _PROC_H */
