/*
 * A20OS — Enhanced Process Management
 *
 * Extends the basic scheduler with:
 *   - Signal state per-process
 *   - wait4() with pid filtering and WNOHANG
 *   - proc_clone() for fork
 *   - proc_exec() for ELF execution
 *   - proc_kill() signal delivery
 *   - mmap/brk virtual memory tracking
 *   - Process name
 */

#include "proc.h"
#include "signal.h"
#include "elf.h"
#include "vfs.h"
#include "mm.h"
#include "frame.h"
#include "vm.h"
#include "trap.h"
#include "timer.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "consts.h"
#include "defs.h"
#include "klog.h"

static task_t proc_table[MAX_PROCS];
static uint64_t *kernel_pgdir_shared;
static task_t *current_task = NULL;

/* Boot stack (for schedule back to idle) */
static uint64_t g_idle_kstack;

void idle_loop(void) {
    while (1) {
        __asm__ volatile("wfi");
        proc_yield();
    }
}

void proc_set_name(task_t *t, const char *name) {
    if (!t) return;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

void proc_init(void) {
    memset(proc_table, 0, sizeof(proc_table));

    task_t *idle = &proc_table[0];
    idle->pid    = 0;
    idle->ppid   = 0;
    idle->state  = PROC_RUNNING;
    idle->cwd[0] = '/';
    idle->cwd[1] = '\0';
    idle->pgid   = 0;
    idle->sid    = 0;
    idle->umask  = 022;
    idle->rlim_nofile = MAX_FILES;
    proc_set_name(idle, "idle");

    vfs_proc_init_fds(idle->fd_table);
    idle->parent  = NULL;

    /* Allocate signal state */
    idle->signals = (struct signal_state *)kmalloc(sizeof(signal_state_t));
    if (idle->signals) signal_init((signal_state_t *)idle->signals);

    void *idle_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle_stack) panic("proc_init: no memory for idle stack");
    memset(idle_stack, 0, KERNEL_STACK_SIZE);

    uint64_t stack_top = (uint64_t)idle_stack + KERNEL_STACK_SIZE;
    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra   = (uint64_t)idle_loop;
    ctx->tp   = (uint64_t)idle;

    uint64_t *kpdir = pt_create();
    if (!kpdir) panic("proc_init: pt_create failed");
    pt_map_kernel(kpdir);
    kernel_pgdir_shared = kpdir;
    idle->pgdir = kpdir;
    ctx->satp = kpdir ? MAKE_SATP(kpdir) : 0;
    ctx->sstatus = SSTATUS_SIE;
    idle->kstack_base = idle_stack;
    idle->kstack = (uint64_t)ctx;
    g_idle_kstack = idle->kstack;

    __asm__ volatile("mv tp, %0" :: "r"(idle));
    current_task = idle;

    kdebug("[PROC] Initialized, idle task pid=0\n");
}

task_t *proc_current(void) { return current_task; }

task_t *proc_find(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid)
            return &proc_table[i];
    }
    return NULL;
}

/* ---- Base task allocation ---- */

static int next_pid = 1;

static task_t *alloc_task_slot(void) {
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) return &proc_table[i];
    }
    return NULL;
}

static void init_task_common(task_t *t, task_t *parent) {
    t->ppid      = parent ? parent->pid : 0;
    t->state     = PROC_READY;
    t->parent    = parent;
    t->exit_code = 0;
    t->priority  = parent ? parent->priority : 0;
    t->wake_time = 0;
    t->total_time = 0;
    t->pgid      = parent ? parent->pgid : t->pid;
    t->sid       = parent ? parent->sid  : t->pid;
    t->umask     = parent ? parent->umask : 022;
    t->rlim_nofile = MAX_FILES;
    t->mm        = NULL;

    if (parent) {
        memcpy(t->cwd, parent->cwd, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
        t->exec_path[0] = '\0';
    }

    for (int i = 0; i < MAX_FILES; i++) t->fd_table[i] = -1;
    if (parent) {
        vfs_proc_copy_fds(parent->fd_table, t->fd_table);
    }
    vfs_proc_init_stdio_defaults(t->fd_table);

    /* Signal state */
    t->signals = kmalloc(sizeof(signal_state_t));
    if (t->signals) {
        if (parent && parent->signals)
            signal_copy((signal_state_t *)parent->signals, (signal_state_t *)t->signals);
        else
            signal_init((signal_state_t *)t->signals);
    }
}

int proc_alloc(void (*entry)(void)) {
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    init_task_common(t, current_task);
    proc_set_name(t, "kthread");

    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        vfs_proc_close_all_fds(t->fd_table);
        if (t->signals) { kfree(t->signals); t->signals = NULL; }
        t->state = PROC_UNUSED;
        return -ENOMEM;
    }
    memset(stack, 0, KERNEL_STACK_SIZE);

    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;

    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra   = (uint64_t)entry;
    ctx->tp   = (uint64_t)t;
    t->pgdir  = kernel_pgdir_shared;
    ctx->satp = kernel_pgdir_shared ? MAKE_SATP(kernel_pgdir_shared) : 0;
    ctx->sstatus = SSTATUS_SIE;
    t->kstack_base = stack;
    t->kstack = (uint64_t)ctx;

    kdebug("[PROC] kthread pid=%d\n", t->pid);
    return t->pid;
}

/* Allocate a user-mode task with given entry point and stack */
int proc_alloc_user(uint64_t entry, uint64_t sp, uint64_t *pgdir) {
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    init_task_common(t, current_task);
    t->entry = entry;
    t->pgdir = pgdir;
    proc_set_name(t, "user");

    void *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        vfs_proc_close_all_fds(t->fd_table);
        if (t->signals) { kfree(t->signals); t->signals = NULL; }
        t->state = PROC_UNUSED;
        return -ENOMEM;
    }
    memset(kstack, 0, KERNEL_STACK_SIZE);
    t->kstack_base = kstack;

    uint64_t ks_top = (uint64_t)kstack + KERNEL_STACK_SIZE;

    trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
    memset(trap, 0, sizeof(*trap));
    trap->sepc   = entry;
    trap->x[0]   = pgdir ? MAKE_SATP(pgdir) : 0;
    trap->x[2]   = sp;
    trap->sstatus = SSTATUS_SPIE | SSTATUS_FS_CLEAN;
    trap->kernel_tp = (uint64_t)(uintptr_t)t;

    t->trap_ctx = trap;
    t->ustack   = sp;
    t->pgdir = pgdir;

    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (mm) {
        mm->pgdir       = pgdir;
        mm->brk         = 0;
        mm->start_brk   = 0;
        mm->mmap_base   = MMAP_BASE_ADDR;
        mm->stack_top   = sp;
        mm->stack_bottom = sp - (INITIAL_STACK_PAGES - 1) * PAGE_SIZE;
        mm->total_vm    = 0;
        mm->rss         = 0;
        mm->refcount    = 1;
        mm->mmap        = NULL;
        t->mm = mm;
    }

    task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    extern void user_trap_return(void);
    ctx->ra   = (uint64_t)user_trap_return;
    ctx->tp   = (uint64_t)t;
    ctx->satp = pgdir ? MAKE_SATP(pgdir) : 0;
    ctx->sstatus = SSTATUS_SIE;
    t->kstack = (uint64_t)ctx;

    kdebug("[PROC] user task pid=%d entry=0x%lx sp=0x%lx pgdir=0x%lx\n", t->pid,
          (unsigned long)entry, (unsigned long)sp,
          (unsigned long)(uintptr_t)pgdir);

    t->state = PROC_READY;
    return t->pid;
}

/* ============================================================
 * Clone (fork)
 * ============================================================ */

int proc_clone(uint64_t flags, uint64_t stack, int *ptid, int *ctid, uint64_t tls) {
    (void)flags; (void)ptid; (void)ctid; (void)tls;

    task_t *parent = current_task;
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    init_task_common(t, parent);
    proc_set_name(t, parent->name);

    t->exec_load_addr = parent->exec_load_addr;
    t->exec_load_size = parent->exec_load_size;

    if (parent->pgdir) {
        if (parent->mm) {
            t->mm = mm_fork(parent->mm);
            if (!t->mm) {
                vfs_proc_close_all_fds(t->fd_table);
                if (t->signals) { kfree(t->signals); t->signals = NULL; }
                t->state = PROC_UNUSED;
                return -ENOMEM;
            }
            t->pgdir = t->mm->pgdir;

        } else {
            /* Kernel thread — just share the kernel page directory */
            t->pgdir = parent->pgdir;
        }
    }

    void *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        if (t->mm) { mm_destroy(t->mm); t->mm = NULL; t->pgdir = NULL; }
        else if (t->pgdir && t->pgdir != kernel_pgdir_shared) { pt_destroy_user(t->pgdir); t->pgdir = NULL; }
        for (int i = 0; i < MAX_FILES; i++) {
            if (t->fd_table[i] >= 0) { vfs_close(t->fd_table[i]); t->fd_table[i] = -1; }
        }
        if (t->signals) { kfree(t->signals); t->signals = NULL; }
        t->state = PROC_UNUSED; return -ENOMEM;
    }
    memset(kstack, 0, KERNEL_STACK_SIZE);
    t->kstack_base = kstack;

    uint64_t ks_top = (uint64_t)kstack + KERNEL_STACK_SIZE;

    if (parent->trap_ctx) {
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        *trap = *parent->trap_ctx;
        trap->x[0]  = t->pgdir ? MAKE_SATP(t->pgdir) : 0;
        trap->x[10] = 0;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;
        if (stack) trap->x[2] = stack;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        extern void user_trap_return(void);
        ctx->ra   = (uint64_t)user_trap_return;
        ctx->tp   = (uint64_t)t;
        ctx->satp = t->pgdir ? MAKE_SATP(t->pgdir) : 0;
        t->kstack = (uint64_t)ctx;
    } else {
        task_context_t *ctx = (task_context_t *)(ks_top - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra   = (uint64_t)idle_loop;
        ctx->tp   = (uint64_t)t;
        t->kstack = (uint64_t)ctx;
    }

    return t->pid;
}

/* ============================================================
 * Exec — replace process image with ELF
 * ============================================================ */

int proc_exec(const char *path, char *const argv[], char *const envp[]) {
    task_t *t = current_task;
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;

    char abs_path[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(abs_path, path, MAX_PATH_LEN - 1);
        abs_path[MAX_PATH_LEN - 1] = '\0';
    } else {
        const char *cwd = t ? t->cwd : "/";
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/') {
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", cwd, path);
        } else {
            snprintf(abs_path, MAX_PATH_LEN, "%s/%s", cwd, path);
        }
    }

    char *k_path = kmalloc(strlen(path) + 1);
    if (k_path) strcpy(k_path, path);

    char *k_argv[32] = {0};
    char *k_envp[32] = {0};
    int argc = 0, envc = 0;

    auto int is_kptr(const void *p) { return (uintptr_t)p >= PAGE_OFFSET; }

    if (argv) {
        while (argc < 31) {
            char *arg;
            if (is_kptr(argv)) {
                arg = argv[argc];
            } else {
                if (copy_from_user(&arg, &argv[argc], sizeof(char*)) < 0) break;
            }
            if (!arg) break;
            k_argv[argc] = kmalloc(MAX_PATH_LEN);
            if (!k_argv[argc]) break;
            if (is_kptr(arg)) {
                strncpy(k_argv[argc], arg, MAX_PATH_LEN - 1);
                k_argv[argc][MAX_PATH_LEN - 1] = '\0';
            } else {
                if (user_strncpy(k_argv[argc], arg, MAX_PATH_LEN) < 0) {
                    kfree(k_argv[argc]); k_argv[argc] = NULL; break;
                }
            }
            argc++;
        }
    }
    if (envp) {
        while (envc < 31) {
            char *env;
            if (is_kptr(envp)) {
                env = envp[envc];
            } else {
                if (copy_from_user(&env, &envp[envc], sizeof(char*)) < 0) break;
            }
            if (!env) break;
            k_envp[envc] = kmalloc(MAX_PATH_LEN);
            if (!k_envp[envc]) break;
            if (is_kptr(env)) {
                strncpy(k_envp[envc], env, MAX_PATH_LEN - 1);
                k_envp[envc][MAX_PATH_LEN - 1] = '\0';
            } else {
                if (user_strncpy(k_envp[envc], env, MAX_PATH_LEN) < 0) {
                    kfree(k_envp[envc]); k_envp[envc] = NULL; break;
                }
            }
            envc++;
        }
    }

    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    int r = elf_load(fd, k_path, &info);
    if (r < 0) {
        if (r == -ENOEXEC) {
            vfs_lseek(fd, 0, SEEK_SET);
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf) - 1);
            if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
                buf[n] = '\0';
                char *cp = buf + 2;
                while (*cp == ' ' || *cp == '\t') ++cp;
                if (*cp != '\0' && *cp != '\n') {
                    char *interp_start = cp;
                    while (*cp && *cp != '\n' && *cp != ' ' && *cp != '\t') ++cp;
                    char interp_path[128];
                    size_t ilen = cp - interp_start;
                    if (ilen > 0 && ilen < sizeof(interp_path)) {
                        memcpy(interp_path, interp_start, ilen);
                        interp_path[ilen] = '\0';

                        char arg_buf[128] = {0};
                        int has_arg = 0;
                        while (*cp == ' ' || *cp == '\t') ++cp;
                        if (*cp && *cp != '\n') {
                            char *arg_start = cp;
                            while (*cp && *cp != '\n') ++cp;
                            size_t alen = cp - arg_start;
                            if (alen > 0 && alen < sizeof(arg_buf)) {
                                memcpy(arg_buf, arg_start, alen);
                                arg_buf[alen] = '\0';
                                has_arg = 1;
                            }
                        }

                        char *new_argv[64];
                        int na = 0;
                        new_argv[na++] = interp_path;
                        if (has_arg) new_argv[na++] = arg_buf;
                        new_argv[na++] = k_path;
                        for (int i = 1; i < argc && na < 63; i++)
                            new_argv[na++] = k_argv[i];
                        new_argv[na] = NULL;

                        vfs_close(fd);
                        int ret = proc_exec(interp_path, new_argv, envp);
                        for(int i=0; i<argc; i++) kfree(k_argv[i]);
                        for(int i=0; i<envc; i++) kfree(k_envp[i]);
                        kfree(k_path);
                        return ret;
                    }
                }
            }
        }
        vfs_close(fd);
        for(int i=0; i<argc; i++) kfree(k_argv[i]);
        for(int i=0; i<envc; i++) kfree(k_envp[i]);
        kfree(k_path);
        return r;
    }
    vfs_close(fd);
    uint64_t sp = elf_setup_stack(info.stack_top, argc,
                                   (char *const *)k_argv,
                                   (char *const *)k_envp, &info);

    for(int i=0; i<argc; i++) kfree(k_argv[i]);
    for(int i=0; i<envc; i++) kfree(k_envp[i]);

    /* Destroy old address space */
    mm_struct_t *old_mm = t->mm;
    t->mm = NULL;

    /* Build fresh mm_struct wrapping the ELF loader's page directory */
    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm) { mm_destroy(old_mm); kfree(k_path); return -ENOMEM; }
    mm->pgdir       = info.pgdir;
    mm->brk         = info.brk;
    mm->start_brk   = info.brk;
    mm->mmap_base   = MMAP_BASE_ADDR;
    mm->stack_top   = info.stack_top;
    mm->stack_bottom = info.stack_top - (INITIAL_STACK_PAGES - 1) * PAGE_SIZE;
    mm->total_vm    = 0;
    mm->rss         = 0;
    mm->refcount    = 1;
    mm->mmap        = info.mmap;
    t->mm           = mm;

    uint64_t *old_pgdir = t->pgdir;
    t->pgdir = info.pgdir;
    t->entry = info.entry;
    t->ustack = sp;
    proc_set_name(t, path);
    strncpy(t->exec_path, abs_path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';

    if (t->signals) signal_init((signal_state_t *)t->signals);
    t->sig_handling = 0;

    uint64_t saved_entry = info.entry;
    uint64_t saved_sp    = sp;

    if (!t->trap_ctx) {
        uint64_t ks_top = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        task_context_t *ctx  = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        extern void user_trap_return(void);
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)(uintptr_t)t;
        ctx->satp = MAKE_SATP(info.pgdir);
        t->trap_ctx = trap;
        t->kstack   = (uint64_t)ctx;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        memset(trap, 0, sizeof(*trap));
        trap->sepc      = saved_entry;
        trap->x[0]      = MAKE_SATP(info.pgdir);
        trap->x[2]      = saved_sp;
        trap->x[4]      = info.tls_tp;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        trap->sstatus   = SSTATUS_SPIE | SSTATUS_FS_CLEAN;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        extern void user_trap_return(void);
        ctx->ra   = (uint64_t)user_trap_return;
        ctx->tp   = (uint64_t)(uintptr_t)t;
        ctx->satp = MAKE_SATP(info.pgdir);
    }

    t->exec_load_addr = info.load_addr;
    t->exec_load_size = info.load_size;

    /* Destroy old address space (page tables + old mm if any) */
    if (old_mm) {
        /* old_mm->pgdir was old_pgdir — mm_destroy handles pt_destroy_user */
        mm_destroy(old_mm);
    } else if (old_pgdir && old_pgdir != kernel_pgdir_shared) {
        pt_destroy_user(old_pgdir);
    }

    kfree(k_path);

    __asm__ volatile("fence.i" ::: "memory");

    t->state = PROC_RUNNING;
    return 0;
}

/* ============================================================
 * Free / Exit
 * ============================================================ */

void proc_free_pid(int pid) {
    task_t *t = proc_find(pid);
    if (!t) return;

    if (t->mm) {
        mm_destroy(t->mm);
        t->mm = NULL;
    }
    t->pgdir = NULL;

    if (t->signals) { kfree(t->signals); t->signals = NULL; }

    if (t->kstack) {
        kfree(t->kstack_base);
        t->kstack = 0;
        t->kstack_base = NULL;
    }

    memset(t, 0, sizeof(*t));
}

void proc_exit(int exit_code) {
    task_t *t = current_task;
    if (!t) panic("proc_exit: no current task");
    /* Close all file descriptors */
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] >= 0) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
    }

    /* Re-parent orphaned children to init (pid 1) or idle */
    task_t *reaper = proc_find(1);
    if (!reaper || reaper == t) reaper = &proc_table[0];
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].ppid == t->pid && proc_table[i].state != PROC_UNUSED) {
            proc_table[i].ppid   = reaper->pid;
            proc_table[i].parent = reaper;
        }
    }

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->state     = PROC_ZOMBIE;

    /* Wake up parent if blocked in wait */
    if (t->parent && t->parent->state == PROC_BLOCKED) {
        t->parent->state = PROC_READY;
    }

    /* Send SIGCHLD to parent */
    if (t->parent) signal_send(t->parent->pid, SIGCHLD);

    sched();
    panic("proc_exit: sched returned");
}

/* ============================================================
 * Wait4
 * ============================================================ */

int proc_wait4(int pid, int *status, int options) {
    task_t *t = current_task;

#define WNOHANG   1

retry:;
    int found = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        found = 1;
        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0x7F;
            }
            int child_pid = child->pid;
            proc_free_pid(child_pid);
            return child_pid;
        }
    }

    if (!found) return -ECHILD;
    if (options & WNOHANG) return 0;

    /* Block until a child exits */
    t->state = PROC_BLOCKED;
    sched();
    goto retry;
}

int proc_wait(int *status) {
    return proc_wait4(-1, status, 0);
}

/* ============================================================
 * Kill
 * ============================================================ */

int proc_kill(int pid, int signum) {
    return signal_send(pid, signum);
}

int proc_kill_pgid(int pgid, int signum, int skip_self) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *self = current_task;
    int count = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *t = &proc_table[i];
        if (t->state == PROC_UNUSED) continue;
        if (t->pgid != pgid) continue;
        if (skip_self && t == self) continue;
        signal_send(t->pid, signum);
        count++;
    }
    return count > 0 ? count : -ESRCH;
}

/* ============================================================
 * Scheduler
 * ============================================================ */

void context_switch(task_t *next) {
    current_task = next;
    next->state  = PROC_RUNNING;
    __switch(next->kstack);
}

void sched(void) {
    uint64_t now = timer_get_ticks();
    int cur_idx  = -1;

    for (int i = 0; i < MAX_PROCS; i++) {
        if (&proc_table[i] == current_task) { cur_idx = i; break; }
    }

    /* Wake up sleeping processes */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_BLOCKED && proc_table[i].wake_time > 0
            && now >= proc_table[i].wake_time) {
            proc_table[i].state     = PROC_READY;
            proc_table[i].wake_time = 0;
        }
    }

    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (cur_idx >= 0) ? (cur_idx + i) % MAX_PROCS : i;
        task_t *t = &proc_table[idx];
        /* Skip current task and UNUSED tasks */
        if (t == current_task || t->state == PROC_UNUSED) continue;
        if (t->state == PROC_READY) {
            context_switch(t);
            return;
        }
    }


    /* If current task is READY (e.g. after execve reconfigured itself or yielded), switch to it */
    if (current_task && current_task->state == PROC_READY) {
        // kinfo("[SCHED] Re-scheduling current task: pid=%d\n", current_task->pid);
        current_task->state = PROC_RUNNING;
        return;
    }

    /* No runnable task — run idle */
    if (current_task != &proc_table[0]) {
        context_switch(&proc_table[0]);
    }
}

void proc_yield(void) {
    if (current_task && current_task->state == PROC_RUNNING)
        current_task->state = PROC_READY;
    sched();
}

/* ============================================================
 * mmap / brk
 * ============================================================ */

uint64_t proc_brk(uint64_t newbrk) {
    task_t *t = current_task;
    if (!t || !t->mm) return -1;
    return mm_brk(t->mm, newbrk);
}

uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    task_t *t = current_task;
    if (!t || !t->mm) return (uint64_t)-1;

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) return (uint64_t)-EINVAL;

    if ((flags & MAP_ANONYMOUS) || fd < 0)
        return mm_mmap(t->mm, addr, len, prot, flags);

    if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;

    uint64_t mapped = mm_mmap(t->mm, addr, len, prot, flags);
    if ((int64_t)mapped < 0)
        return mapped;

    long saved_off = vfs_lseek(fd, 0, SEEK_CUR);
    if (saved_off < 0) {
        mm_munmap(t->mm, mapped, map_len);
        return (uint64_t)-EIO;
    }
    if (vfs_lseek(fd, off, SEEK_SET) < 0) {
        mm_munmap(t->mm, mapped, map_len);
        return (uint64_t)-EIO;
    }

    uint64_t pte_flags = prot_to_pte(prot);
    for (size_t done = 0; done < map_len; done += PAGE_SIZE) {
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) {
            vfs_lseek(fd, saved_off, SEEK_SET);
            mm_munmap(t->mm, mapped, map_len);
            return (uint64_t)-ENOMEM;
        }

        char *page = pfn_to_virt(pfn);
        memset(page, 0, PAGE_SIZE);

        size_t want = 0;
        if (done < len) {
            want = len - done;
            if (want > PAGE_SIZE)
                want = PAGE_SIZE;
        }

        if (want > 0) {
            size_t copied = 0;
            while (copied < want) {
                int nr = vfs_read(fd, page + copied, want - copied);
                if (nr < 0) {
                    frame_put(pfn);
                    vfs_lseek(fd, saved_off, SEEK_SET);
                    mm_munmap(t->mm, mapped, map_len);
                    return (uint64_t)-EIO;
                }
                if (nr == 0)
                    break;
                copied += (size_t)nr;
            }
        }

        int r = pt_map(t->mm->pgdir, mapped + done, pfn_to_phys(pfn), pte_flags);
        if (r < 0) {
            frame_put(pfn);
            vfs_lseek(fd, saved_off, SEEK_SET);
            mm_munmap(t->mm, mapped, map_len);
            return (uint64_t)r;
        }

        t->mm->rss++;
    }

    vfs_lseek(fd, saved_off, SEEK_SET);
    __asm__ volatile("sfence.vma" ::: "memory");
    return mapped;
}

int proc_munmap(uint64_t addr, size_t len) {
    task_t *t = current_task;
    if (!t || !t->mm) return -1;
    return mm_munmap(t->mm, addr, len);
}

void proc_dump(void) {
    printf("  PID  PPID  STATE  PRI  NAME\n");
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) continue;
        const char *s = "?";
        switch (proc_table[i].state) {
            case PROC_READY:   s = "RDY"; break;
            case PROC_RUNNING: s = "RUN"; break;
            case PROC_BLOCKED: s = "BLK"; break;
            case PROC_ZOMBIE:  s = "ZOM"; break;
            default: break;
        }
        printf("  %3d   %3d   %s   %3d  %s\n",
               proc_table[i].pid, proc_table[i].ppid, s,
               proc_table[i].priority, proc_table[i].name);
    }
}
