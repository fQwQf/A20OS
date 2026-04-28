#ifndef _PROC_H
#define _PROC_H

#include "core/types.h"
#include "core/consts.h"
#include "core/trap.h"
#include "core/defs.h"

struct signal_state;
struct mm_struct;
struct vm_area;
typedef struct mm_struct mm_struct_t;

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
    int      fd_table[MAX_FILES];
    uint8_t  fd_cloexec[MAX_FILES];
    char     cwd[MAX_PATH_LEN];
    char     root_path[MAX_PATH_LEN];
    struct task_t *parent;
    uint64_t wake_time;
    uint64_t alarm_expire;
    uint64_t itimer_real_interval;
    uint64_t itimer_values[3][4];
    int      priority;
    int      sched_level;
    int      on_rq;
    struct task_t *rq_next;
    struct task_t *rq_prev;
    uint64_t total_time;

    struct signal_state *signals;

    mm_struct_t *mm;

    uint64_t  entry;
    uint64_t  exec_load_addr;
    size_t    exec_load_size;

    int       pgid;
    int       sid;

    uint64_t  rlim_stack;
    uint64_t  rlim_nofile;

    int       umask;

    int       uid;
    int       euid;
    int       suid;
    int       fsuid;
    int       gid;
    int       egid;
    int       sgid;
    int       fsgid;
    int       ngroups;
    int       groups[MAX_GROUPS];
    uint64_t  cap_effective;
    uint64_t  cap_permitted;
    uint64_t  cap_inheritable;
    uint64_t  cap_bounding;
    int       oom_score_adj;
    int       thp_disabled;
    int       clone_flags;
    int      *clear_child_tid;

    char      name[64];
    char      exec_path[MAX_PATH_LEN];
    struct task_t *pid_hash_next;

    trap_context_t sig_saved_ctx;
    uint64_t       sig_old_blocked;
    int            sig_handling;
    uint64_t       sigsuspend_old_blocked;
    int            sigsuspend_active;
} task_t;

/* ---- Process management API ---- */
void     proc_init(void);
void     idle_loop(void) NORETURN;
task_t  *proc_current(void);
task_t  *proc_find(int pid);
int      proc_pid_max(void);
int      proc_set_pid_max(int value);
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
void     proc_dump(void);
int      proc_kill(int pid, int signum);
int      proc_kill_pgid(int pgid, int signum, int skip_self);
void     proc_set_name(task_t *t, const char *name);
void     proc_make_ready(task_t *t);

/* For execve: replace current process image */
int      proc_exec(const char *path, char *const argv[], char *const envp[]);

/* mmap/brk helpers */
uint64_t proc_brk(uint64_t newbrk);
uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off);
int      proc_munmap(uint64_t addr, size_t len);

/* Clone (fork-like) */
int      proc_clone(uint64_t flags, uint64_t stack, int *ptid, uint64_t tls, int *ctid);

#endif /* _PROC_H */
