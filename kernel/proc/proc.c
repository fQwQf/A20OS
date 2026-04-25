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

#include "proc/proc.h"
#include "proc/signal.h"
#include "mm/elf.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "core/trap.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"

static task_t proc_table[MAX_PROCS];  // 进程表
static uint64_t *kernel_pgdir_shared;  // 共享的内核页表
static task_t *current_task = NULL;    // 当前运行的任务
static int wait_diag_count = 0;
static int sched_diag_count = 0;
static int fork_diag_count = 0;

/* Boot stack (for schedule back to idle) */
static uint64_t g_idle_kstack;  // idle 进程的内核栈

// idle 进程的主循环，系统无任务时运行
void idle_loop(void) {
    while (1) {
        arch_wfi();  // 等待中断
        proc_yield();              // 主动让出 CPU
    }
}

// 设置进程名称
void proc_set_name(task_t *t, const char *name) {
    if (!t) return;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->name[sizeof(t->name) - 1] = '\0';
}

// 初始化进程管理模块，创建 idle 进程
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
    idle->rlim_stack = USER_STACK_MAX_SIZE;
    idle->rlim_nofile = MAX_FILES;
    proc_set_name(idle, "idle");

    vfs_proc_init_fds(idle->fd_table);  // 初始化文件描述符
    idle->parent  = NULL;

    /* Allocate signal state */
    idle->signals = (struct signal_state *)kmalloc(sizeof(signal_state_t));
    if (idle->signals) signal_init((signal_state_t *)idle->signals);

    // 分配内核栈
    void *idle_stack = kmalloc(KERNEL_STACK_SIZE);
    if (!idle_stack) panic("proc_init: no memory for idle stack");
    memset(idle_stack, 0, KERNEL_STACK_SIZE);

    // 设置任务上下文
    uint64_t stack_top = (uint64_t)idle_stack + KERNEL_STACK_SIZE;
    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra   = (uint64_t)idle_loop;  // 返回地址为 idle_loop
    ctx->tp   = (uint64_t)idle;       // tp 寄存器指向任务结构

    // 创建并映射内核页表
    uint64_t *kpdir = pt_create();
    if (!kpdir) panic("proc_init: pt_create failed");
    pt_map_kernel(kpdir);
    kernel_pgdir_shared = kpdir;
    idle->pgdir = kpdir;
    TASK_CTX_PAGE_TABLE(ctx) = kpdir ? arch_make_satp(kpdir) : 0;
    TASK_CTX_STATUS(ctx) = SSTATUS_SIE;  // 启用中断
    idle->kstack_base = idle_stack;
    idle->kstack = (uint64_t)ctx;
    g_idle_kstack = idle->kstack;

    arch_set_task_pointer(idle);  // 设置 tp 寄存器
    current_task = idle;

    kdebug("[PROC] Initialized, idle task pid=0\n");
}

// 获取当前运行的任务
task_t *proc_current(void) { return current_task; }

// 根据 PID 查找进程
task_t *proc_find(int pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].pid == pid)
            return &proc_table[i];
    }
    return NULL;
}

/* ---- Base task allocation ---- */

static int next_pid = 1;  // 下一个可用的 PID

// 分配一个空闲的任务槽
static task_t *alloc_task_slot(void) {
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) return &proc_table[i];
    }
    return NULL;
}

// 初始化任务的公共字段
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
    t->rlim_stack = parent ? parent->rlim_stack : USER_STACK_MAX_SIZE;
    t->rlim_nofile = parent ? parent->rlim_nofile : MAX_FILES;
    t->mm        = NULL;

    // 继承父进程的工作目录和执行路径
    if (parent) {
        memcpy(t->cwd, parent->cwd, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
        t->exec_path[0] = '\0';
    }

    // 处理文件描述符
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

// 分配一个内核线程
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
    TASK_CTX_PAGE_TABLE(ctx) = kernel_pgdir_shared ? arch_make_satp(kernel_pgdir_shared) : 0;
    TASK_CTX_STATUS(ctx) = SSTATUS_SIE;
    t->kstack_base = stack;
    t->kstack = (uint64_t)ctx;

    kdebug("[PROC] kthread pid=%d\n", t->pid);
    return t->pid;
}

/* Allocate a user-mode task with given entry point and stack */
// 分配一个用户态任务
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
    TRAP_CTX_EPC(trap)   = entry;
    TRAP_CTX_SP(trap)   = sp;
    TRAP_CTX_STATUS(trap) = SSTATUS_SPIE | SSTATUS_FS_CLEAN;
    TRAP_CTX_KScratch0(trap) = pgdir ? arch_make_satp(pgdir) : 0;
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
        mm->stack_bottom = sp - (USER_STACK_INITIAL_PAGES - 1) * PAGE_SIZE;
        mm->total_vm    = 0;
        mm->rss         = 0;
        mm->refcount    = 1;
        mm->mmap        = NULL;
        t->mm = mm;
    }

    task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra   = (uint64_t)user_trap_return;
    ctx->tp   = (uint64_t)t;
    TASK_CTX_PAGE_TABLE(ctx) = pgdir ? arch_make_satp(pgdir) : 0;
    TASK_CTX_STATUS(ctx) = SSTATUS_SPIE | SSTATUS_FS_CLEAN;
    t->kstack = (uint64_t)ctx;

    kinfo("[PROC] user task pid=%d entry=0x%lx sp=0x%lx\n", t->pid,
          (unsigned long)entry, (unsigned long)sp);

    t->state = PROC_READY;
    return t->pid;
}

/* ============================================================
 * Clone (fork)
 * ============================================================ */

// 克隆当前进程（fork 系统调用的实现）
int proc_clone(uint64_t flags, uint64_t stack, int *ptid, int *ctid, uint64_t tls) {
    (void)flags;
    (void)ptid; (void)ctid; (void)tls;

    task_t *parent = current_task;
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    memset(t, 0, sizeof(*t));
    t->pid = next_pid++;
    init_task_common(t, parent);
    proc_set_name(t, parent->name);
    if (fork_diag_count < 128) {
        fork_diag_count++;
        kdebug("[FORKDBG] parent=%d child=%d flags=0x%lx stack=0x%lx\n",
              parent ? parent->pid : -1, t->pid,
              (unsigned long)flags, (unsigned long)stack);
    }

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
        TRAP_CTX_ARG0(trap) = 0;
        TRAP_CTX_KScratch0(trap) = t->pgdir ? arch_make_satp(t->pgdir) : 0;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;
        if (stack) TRAP_CTX_SP(trap) = stack;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        ctx->ra   = (uint64_t)user_trap_return;
        ctx->tp   = (uint64_t)t;
        TASK_CTX_PAGE_TABLE(ctx) = t->pgdir ? arch_make_satp(t->pgdir) : 0;
        TASK_CTX_STATUS(ctx) = TRAP_CTX_STATUS(trap);
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

// 执行新的程序（execve 系统调用的实现）
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

    auto int is_kptr(const void *p) {
        return arch_is_kernel_address(p);
    }

    int argv_is_kernel = argv && is_kptr(argv);
    if (argv) {
        while (argc < 31) {
            char *arg;
            if (argv_is_kernel) {
                arg = argv[argc];
            } else {
                if (copy_from_user(&arg, &argv[argc], sizeof(char*)) < 0) break;
            }
            if (!arg) break;
            k_argv[argc] = kmalloc(MAX_PATH_LEN);
            if (!k_argv[argc]) break;
            if (argv_is_kernel) {
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
    int envp_is_kernel = envp && is_kptr(envp);
    if (envp) {
        while (envc < 31) {
            char *env;
            if (envp_is_kernel) {
                env = envp[envc];
            } else {
                if (copy_from_user(&env, &envp[envc], sizeof(char*)) < 0) break;
            }
            if (!env) break;
            k_envp[envc] = kmalloc(MAX_PATH_LEN);
            if (!k_envp[envc]) break;
            if (envp_is_kernel) {
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
    kdebug("[EXEC] elf_load returned %d entry=0x%lx pgdir=0x%lx stack=0x%lx\n",
           r, info.entry, (uint64_t)info.pgdir, info.stack_top);
    if (r < 0) {
        if (r == -ENOEXEC) {
            vfs_lseek(fd, 0, SEEK_SET);
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf) - 1);
            if (n >= 2 && buf[0] == '#' && buf[1] == '!') { // 有必要吗？也许shell会完成
                buf[n] = '\0';
                char *cp = buf + 2;
                while (*cp == ' ' || *cp == '\t') ++cp;
                if (*cp != '\0' && *cp != '\n') {
                    char *interp_start = cp;
                    while (*cp && *cp != '\n' && *cp != '\r' && *cp != ' ' && *cp != '\t') ++cp;
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
                            while (*cp && *cp != '\n' && *cp != '\r') ++cp;
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
    kdebug("[EXEC] setup_stack sp=0x%lx argc=%d\n", sp, argc);

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
    mm->stack_bottom = info.stack_top - (USER_STACK_INITIAL_PAGES - 1) * PAGE_SIZE;
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
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)(uintptr_t)t;
        TASK_CTX_PAGE_TABLE(ctx) = arch_make_satp(info.pgdir);
        t->trap_ctx = trap;
        t->kstack   = (uint64_t)ctx;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        memset(trap, 0, sizeof(*trap));
        TRAP_CTX_KScratch0(trap) = arch_make_satp(info.pgdir);
        TRAP_CTX_EPC(trap)      = saved_entry;
        TRAP_CTX_SP(trap)      = saved_sp;
        TRAP_CTX_TP(trap)      = info.tls_tp;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        TRAP_CTX_STATUS(trap)   = SSTATUS_SPIE | SSTATUS_FS_CLEAN;

        /* Do NOT rewrite task_context_t here — it overlaps with our
         * own call frame (proc_exec → sys_execve → trap_handler).
         * The task_context was already set up correctly during fork
         * (or the first-time branch above) and is no longer consumed
         * by the context switcher once the task is running. */
    }

    t->exec_load_addr = info.load_addr;
    t->exec_load_size = info.load_size;

    kdebug("[EXEC] about to switch satp pgdir=0x%lx old_mm=0x%lx\n", (uint64_t)info.pgdir, (uint64_t)old_mm);
    arch_write_satp(arch_make_satp(info.pgdir));
    arch_tlb_flush();
    kdebug("[EXEC] satp switched OK, old_mm=0x%lx\n", (uint64_t)old_mm);

    /* On RISC-V the CPU fetches instructions through satp,
     * so we must switch BEFORE destroying old page tables. */
    if (old_mm) {
        kdebug("[EXEC] mm_destroy enter\n");
        mm_destroy(old_mm);
        kdebug("[EXEC] mm_destroy done\n");
    } else if (old_pgdir && old_pgdir != kernel_pgdir_shared) {
        pt_destroy_user(old_pgdir);
    }

    kdebug("[EXEC] kfree k_path=%p\n", k_path);
    kfree(k_path);
    kdebug("[EXEC] fence_i\n");
    arch_fence_i();

    kdebug("[EXEC] return entry=0x%lx sp=0x%lx\n", saved_entry, saved_sp);
    t->state = PROC_RUNNING;
    return 0;
}

/* ============================================================
 * Free / Exit
 * ============================================================ */

// 释放指定 PID 的进程资源
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

// 进程退出（exit 系统调用的实现）
// SIGCHLD
void proc_exit(int exit_code) {
    task_t *t = current_task;
    if (!t) panic("proc_exit: no current task");
    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] exit pid=%d ppid=%d code=%d state=%d\n",
              t->pid, t->ppid, exit_code, t->state);
    }
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

// 等待子进程结束（wait4 系统调用的实现）
int proc_wait4(int pid, int *status, int options) {
    task_t *t = current_task;

#define WNOHANG   1

    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] enter cur=%d pid=%d opt=0x%x\n",
               t ? t->pid : -1, pid, options);
    }

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
            if (wait_diag_count < 128) {
                wait_diag_count++;
                kdebug("[WAITDBG] reap cur=%d child=%d status=0x%x\n",
                      t ? t->pid : -1, child_pid, status ? *status : code);
            }
            proc_free_pid(child_pid);
            return child_pid;
        }
    }

    if (!found) {
        if (wait_diag_count < 128) {
            wait_diag_count++;
            kdebug("[WAITDBG] no-child cur=%d pid=%d\n",
                  t ? t->pid : -1, pid);
        }
        return -ECHILD;
    }
    // 设置WNOHANG就非阻塞wait
    // 非阻塞wait还叫wait吗？
    if (options & WNOHANG) {
        if (wait_diag_count < 128) {
            wait_diag_count++;
            kdebug("[WAITDBG] nohang cur=%d pid=%d\n",
                  t ? t->pid : -1, pid);
        }
        return 0;
    }

    /*
     * Avoid the classic lost-wakeup race:
     * a child can exit after the scan above but before we actually block.
     * Mark ourselves blocked first, then rescan once before scheduling.
     */
    t->state = PROC_BLOCKED;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] block cur=%d pid=%d\n",
              t ? t->pid : -1, pid);
    }

    for (int i = 0; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        int cstate = __atomic_load_n(&child->state, __ATOMIC_ACQUIRE);
        if (cstate == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid == 0 && child->pgid != t->pgid) continue;
        if (pid < -1 && child->pgid != (-pid)) continue;

        if (cstate == PROC_ZOMBIE) {
            int code = __atomic_load_n(&child->exit_code, __ATOMIC_ACQUIRE);
            t->state = PROC_RUNNING;
            if (status) {
                if (code >= 0)
                    *status = (code & 0xFF) << 8;
                else
                    *status = (-code) & 0x7F;
            }
            int child_pid = child->pid;
            if (wait_diag_count < 128) {
                wait_diag_count++;
                kdebug("[WAITDBG] reap-race cur=%d child=%d status=0x%x\n",
                      t ? t->pid : -1, child_pid, status ? *status : code);
            }
            proc_free_pid(child_pid);
            return child_pid;
        }
    }

    if (t->state == PROC_BLOCKED)
        sched();
    t->state = PROC_RUNNING;
    if (wait_diag_count < 128) {
        wait_diag_count++;
        kdebug("[WAITDBG] wake cur=%d pid=%d\n",
              t ? t->pid : -1, pid);
    }
    goto retry;
}

int proc_wait(int *status) {
    return proc_wait4(-1, status, 0);
}

/* ============================================================
 * Kill
 * ============================================================ */

// 向指定进程发送信号（kill 系统调用的实现）
int proc_kill(int pid, int signum) {
    return signal_send(pid, signum);
}

int proc_kill_pgid(int pgid, int signum, int skip_self) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *self = current_task;
    int count = 0;
    // 向进程组中的所有进程发送信号
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

// 上下文切换到指定任务
void context_switch(task_t *next) {
    current_task = next;
    next->state  = PROC_RUNNING;
    __switch(next->kstack);
}

// 调度器：选择下一个运行的任务
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
            if (sched_diag_count < 128) {
                sched_diag_count++;
                kdebug("[SCHEDDBG] wake pid=%d now=%lu wake=%lu\n",
                      proc_table[i].pid,
                      (unsigned long)now,
                      (unsigned long)proc_table[i].wake_time);
            }
            proc_table[i].state     = PROC_READY;
            proc_table[i].wake_time = 0;
        }
    }

    for (int i = 1; i <= MAX_PROCS; i++) {
        int idx = (cur_idx >= 0) ? (cur_idx + i) % MAX_PROCS : i;
        task_t *t = &proc_table[idx];
        /* Idle is a fallback-only task; do not round-robin into it. */
        if (t == &proc_table[0]) continue;
        /* Skip current task and UNUSED tasks */
        if (t == current_task || t->state == PROC_UNUSED) continue;
        if (t->state == PROC_READY) {
            if (sched_diag_count < 256) {
                sched_diag_count++;
                kdebug("[SCHEDDBG] pick cur=%d next=%d state=%d\n",
                      current_task ? current_task->pid : -1, t->pid, t->state);
            }
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

// 主动让出 CPU（yield 系统调用的实现）
void proc_yield(void) {
    if (current_task && current_task != &proc_table[0] &&
        current_task->state == PROC_RUNNING)
        current_task->state = PROC_READY;
    sched();
}

/* ============================================================
 * mmap / brk
 * ============================================================ */

// 调整堆大小（brk 系统调用的实现）
uint64_t proc_brk(uint64_t newbrk) {
    task_t *t = current_task;
    if (!t || !t->mm) return 0; // 理论上不应发生

    // 如果 newbrk 为 0，通常是 C 库在查询当前堆位置
    if (newbrk == 0) return t->mm->brk;

    // mm_brk 内部已经处理了 newbrk < start_brk 的情况（返回旧 brk）
    // 同时也处理了分配失败的情况
    return mm_brk(t->mm, newbrk);
}

// 创建内存映射（mmap 系统调用的实现）
uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    (void)fd; (void)off;
    task_t *t = current_task;
    if (!t || !t->mm) return (uint64_t)-1;
    return mm_mmap(t->mm, addr, len, prot, flags);
}

// 取消内存映射（munmap 系统调用的实现）
int proc_munmap(uint64_t addr, size_t len) {
    task_t *t = current_task;
    if (!t || !t->mm) return -1;
    return mm_munmap(t->mm, addr, len);
}

// 打印所有进程信息
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
