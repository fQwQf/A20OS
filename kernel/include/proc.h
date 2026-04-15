#ifndef _PROC_H
#define _PROC_H

#include "types.h"
#include "consts.h"
#include "trap.h"
#include "defs.h"

/* Forward declaration */
struct signal_state;
struct vm_area;

/* ---- vm_area: Virtual memory area mapping ---- */
typedef struct vm_area {
    uint64_t        start;
    uint64_t        end;
    int             prot;       /* PROT_* flags */
    int             flags;      /* MAP_* flags */
    struct vm_area *next;
} vm_area_t;

typedef struct task_t {
    uint64_t kstack;
    void    *kstack_base;
    int      pid;
    int      ppid;
    proc_state_t state;
    vaddr_t  ustack;
    uint64_t *pgdir;
    trap_context_t *trap_ctx;
    int      exit_code;
    int      fd_table[MAX_FILES];
    char     cwd[MAX_PATH_LEN];
    struct task_t *parent;
    uint64_t wake_time;
    int      priority;
    uint64_t total_time;

    struct signal_state *signals;

    vm_area_t *vm_areas;
    uint64_t   brk;
    uint64_t   mmap_base;

    uint64_t  entry;
    uint64_t  exec_load_addr;
    size_t    exec_load_size;

    int       pgid;
    int       sid;

    uint64_t  rlim_stack;
    uint64_t  rlim_nofile;

    int       umask;

    char      name[64];
    char      exec_path[MAX_PATH_LEN];

    trap_context_t sig_saved_ctx;
    uint64_t       sig_old_blocked;
    int            sig_handling;
} task_t;

/* ---- Process management API ---- */
void     proc_init(void);
void     idle_loop(void) NORETURN;
task_t  *proc_current(void);
task_t  *proc_find(int pid);
int      proc_alloc(void (*entry)(void));
int      proc_alloc_user(uint64_t entry, uint64_t sp, uint64_t *pgdir);
void     proc_free_pid(int pid);
void     proc_exit(int exit_code) NORETURN;
int      proc_wait4(int pid, int *status, int options);
void     proc_yield(void);
void     sched(void);
void     context_switch(task_t *next);
void     proc_dump(void);
int      proc_kill(int pid, int signum);
void     proc_set_name(task_t *t, const char *name);

/* For execve: replace current process image */
int      proc_exec(const char *path, char *const argv[], char *const envp[]);

/* mmap/brk helpers */
uint64_t proc_brk(uint64_t newbrk);
uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off);
int      proc_munmap(uint64_t addr, size_t len);

/* Clone (fork-like) */
int      proc_clone(uint64_t flags, uint64_t stack, int *ptid, int *ctid, uint64_t tls);

#endif /* _PROC_H */
