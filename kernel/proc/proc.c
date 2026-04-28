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
#include "core/lock.h"
#include "drv/virtio_blk.h"
#include "net/lwip_stack.h"
#include "sys/futex.h"

static task_t proc_table[MAX_PROCS];  // 进程表
static uint64_t *kernel_pgdir_shared;  // 共享的内核页表
static task_t *current_task = NULL;    // 当前运行的任务
static int wait_diag_count = 0;
static int sched_diag_count = 0;
static int fork_diag_count = 0;

#define SCHED_LEVELS 8
#define PID_HASH_BITS 8
#define PID_HASH_SIZE (1U << PID_HASH_BITS)

#define CLONE_VM             0x00000100
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

static spinlock_t proc_lock = SPINLOCK_INIT;
static task_t *runq_head[SCHED_LEVELS];
static task_t *runq_tail[SCHED_LEVELS];
static uint32_t runq_bitmap;
static int next_pid = 1;  // 下一个可用的 PID
static int pid_max = 32768;
static task_t *pid_hash[PID_HASH_SIZE];

/* Boot stack (for schedule back to idle) */
static uint64_t g_idle_kstack;  // idle 进程的内核栈

static void proc_destroy_task(task_t *t);
static void proc_reparent_children(task_t *dead, task_t *reaper);

static int sched_level_clamp(int level) {
    if (level < 0) return 0;
    if (level >= SCHED_LEVELS) return SCHED_LEVELS - 1;
    return level;
}

static unsigned pid_hash_index(int pid) {
    return ((unsigned)pid) & (PID_HASH_SIZE - 1);
}

static void pid_hash_insert(task_t *t) {
    if (!t) return;
    unsigned h = pid_hash_index(t->pid);
    t->pid_hash_next = pid_hash[h];
    pid_hash[h] = t;
}

static void pid_hash_remove(task_t *t) {
    if (!t) return;
    unsigned h = pid_hash_index(t->pid);
    task_t **pp = &pid_hash[h];
    while (*pp) {
        if (*pp == t) {
            *pp = t->pid_hash_next;
            t->pid_hash_next = NULL;
            return;
        }
        pp = &(*pp)->pid_hash_next;
    }
}

static int pid_in_use_locked(int pid) {
    task_t *t = pid_hash[pid_hash_index(pid)];
    while (t) {
        if (t->pid == pid && t->state != PROC_UNUSED)
            return 1;
        t = t->pid_hash_next;
    }
    return 0;
}

static int proc_copy_to_task_user(task_t *task, void *dst, const void *src, size_t n)
{
    if (!task || !task->pgdir) return -EFAULT;
    size_t done = 0;
    while (done < n) {
        vaddr_t va = (vaddr_t)(uintptr_t)dst + done;
        paddr_t pa = pt_translate(task->pgdir, va);
        if (!pa) return -EFAULT;
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn)) return -EFAULT;
        char *kv = (char *)pfn_to_virt(pfn) + (pa & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n - done) chunk = n - done;
        memcpy(kv, (const char *)src + done, chunk);
        done += chunk;
    }
    return 0;
}

static void runq_enqueue_locked(task_t *t) {
    if (!t || t == &proc_table[0] || t->state != PROC_READY || t->on_rq)
        return;

    int q = sched_level_clamp(t->sched_level);
    t->sched_level = q;
    t->rq_next = NULL;
    t->rq_prev = runq_tail[q];
    if (runq_tail[q])
        runq_tail[q]->rq_next = t;
    else
        runq_head[q] = t;
    runq_tail[q] = t;
    t->on_rq = 1;
    runq_bitmap |= (1U << q);
}

static void runq_remove_locked(task_t *t) {
    if (!t || !t->on_rq)
        return;

    int q = sched_level_clamp(t->sched_level);
    if (t->rq_prev)
        t->rq_prev->rq_next = t->rq_next;
    else
        runq_head[q] = t->rq_next;
    if (t->rq_next)
        t->rq_next->rq_prev = t->rq_prev;
    else
        runq_tail[q] = t->rq_prev;
    if (!runq_head[q])
        runq_bitmap &= ~(1U << q);

    t->rq_next = NULL;
    t->rq_prev = NULL;
    t->on_rq = 0;
}

static task_t *runq_pick_locked(void) {
    while (runq_bitmap) {
        int q = 0;
        while (q < SCHED_LEVELS && !(runq_bitmap & (1U << q)))
            q++;
        if (q >= SCHED_LEVELS)
            return NULL;

        task_t *t = runq_head[q];
        if (!t) {
            runq_bitmap &= ~(1U << q);
            continue;
        }

        runq_head[q] = t->rq_next;
        if (t->rq_next)
            t->rq_next->rq_prev = NULL;
        else
            runq_tail[q] = NULL;
        if (!runq_head[q])
            runq_bitmap &= ~(1U << q);

        t->rq_next = NULL;
        t->rq_prev = NULL;
        t->on_rq = 0;

        if (t->state == PROC_READY && t->kstack)
            return t;
    }
    return NULL;
}

static int proc_alloc_pid(void) {
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int limit = pid_max > 0 ? pid_max : 1;
    int pid = -EAGAIN;
    for (int i = 0; i < limit; i++) {
        if (next_pid < 1 || next_pid > limit)
            next_pid = 1;
        int candidate = next_pid++;
        if (!pid_in_use_locked(candidate)) {
            pid = candidate;
            break;
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return pid;
}

int proc_pid_max(void) {
    return pid_max;
}

int proc_set_pid_max(int value) {
    if (value < 1 || value > 4194304)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    pid_max = value;
    if (next_pid < 1 || next_pid > pid_max)
        next_pid = 1;
    spin_unlock_irqrestore(&proc_lock, flags);
    return 0;
}

static void proc_register_pid(task_t *t) {
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    pid_hash_insert(t);
    spin_unlock_irqrestore(&proc_lock, flags);
}

static void proc_reap_detached_zombies(void) {
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *t = &proc_table[i];
        if (t->state == PROC_ZOMBIE &&
            (t->ppid == 0 || t->parent == &proc_table[0]))
            proc_destroy_task(t);
    }
}

void proc_make_ready(task_t *t) {
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state != PROC_READY) {
        t->state = PROC_READY;
        if (t->wake_time == 0 && t->sched_level > 0)
            t->sched_level--;
    }
    runq_enqueue_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);
}

static int proc_ignores_sigchld(task_t *parent) {
    if (!parent || !parent->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)parent->signals;
    return ss->actions[SIGCHLD].sa_handler == SIG_IGN;
}

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
    memset(runq_head, 0, sizeof(runq_head));
    memset(runq_tail, 0, sizeof(runq_tail));
    memset(pid_hash, 0, sizeof(pid_hash));
    runq_bitmap = 0;
    spin_init(&proc_lock);

    task_t *idle = &proc_table[0];
    idle->pid    = 0;
    idle->ppid   = 0;
    idle->state  = PROC_RUNNING;
    idle->cwd[0] = '/';
    idle->cwd[1] = '\0';
    idle->root_path[0] = '/';
    idle->root_path[1] = '\0';
    idle->pgid   = 0;
    idle->sid    = 0;
    idle->umask  = 022;
    idle->uid    = 0;
    idle->euid   = 0;
    idle->suid   = 0;
    idle->fsuid  = 0;
    idle->gid    = 0;
    idle->egid   = 0;
    idle->sgid   = 0;
    idle->fsgid  = 0;
    idle->ngroups = 0;
    idle->cap_effective = ~(uint64_t)0;
    idle->cap_permitted = ~(uint64_t)0;
    idle->cap_inheritable = 0;
    idle->cap_bounding = ~(uint64_t)0;
    idle->oom_score_adj = 0;
    idle->thp_disabled = 0;
    idle->rlim_stack = USER_STACK_MAX_SIZE;
    idle->rlim_nofile = MAX_FILES;
    idle->sched_level = SCHED_LEVELS - 1;
    proc_set_name(idle, "idle");
    pid_hash_insert(idle);

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
    task_t *t = pid_hash[pid_hash_index(pid)];
    while (t) {
        if (t->pid == pid && t->state != PROC_UNUSED)
            return t;
        t = t->pid_hash_next;
    }
    return NULL;
}

/* ---- Base task allocation ---- */

// 分配一个空闲的任务槽
static task_t *alloc_task_slot(void) {
    proc_reap_detached_zombies();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (int i = 1; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_UNUSED) {
            memset(&proc_table[i], 0, sizeof(proc_table[i]));
            proc_table[i].state = PROC_BLOCKED;
            spin_unlock_irqrestore(&proc_lock, flags);
            return &proc_table[i];
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return NULL;
}

// 初始化任务的公共字段
static void init_task_common(task_t *t, task_t *parent) {
    t->ppid      = parent ? parent->pid : 0;
    t->tgid      = parent ? parent->tgid : t->pid;
    t->state     = PROC_READY;
    t->parent    = parent;
    t->exit_code = 0;
    t->priority  = parent ? parent->priority : 0;
    t->sched_level = parent ? parent->sched_level : 0;
    t->on_rq     = 0;
    t->rq_next   = NULL;
    t->rq_prev   = NULL;
    t->wake_time = 0;
    t->alarm_expire = 0;
    t->itimer_real_interval = 0;
    memset(t->itimer_values, 0, sizeof(t->itimer_values));
    t->total_time = 0;
    t->pgid      = parent ? parent->pgid : t->pid;
    t->sid       = parent ? parent->sid  : t->pid;
    t->umask     = parent ? parent->umask : 022;
    t->uid       = parent ? parent->uid : 0;
    t->euid      = parent ? parent->euid : 0;
    t->suid      = parent ? parent->suid : 0;
    t->fsuid     = parent ? parent->fsuid : t->euid;
    t->gid       = parent ? parent->gid : 0;
    t->egid      = parent ? parent->egid : 0;
    t->sgid      = parent ? parent->sgid : 0;
    t->fsgid     = parent ? parent->fsgid : t->egid;
    t->ngroups   = parent ? parent->ngroups : 0;
    if (parent)
        memcpy(t->groups, parent->groups, sizeof(t->groups));
    t->cap_effective = parent ? parent->cap_effective : ~(uint64_t)0;
    t->cap_permitted = parent ? parent->cap_permitted : ~(uint64_t)0;
    t->cap_inheritable = parent ? parent->cap_inheritable : 0;
    t->cap_bounding = parent ? parent->cap_bounding : ~(uint64_t)0;
    t->oom_score_adj = parent ? parent->oom_score_adj : 0;
    t->thp_disabled = parent ? parent->thp_disabled : 0;
    t->clone_flags = 0;
    t->clear_child_tid = NULL;
    t->rlim_stack = parent ? parent->rlim_stack : USER_STACK_MAX_SIZE;
    t->rlim_nofile = parent ? parent->rlim_nofile : MAX_FILES;
    t->mm        = NULL;

    // 继承父进程的工作目录和执行路径
    if (parent) {
        memcpy(t->cwd, parent->cwd, MAX_PATH_LEN);
        memcpy(t->root_path, parent->root_path, MAX_PATH_LEN);
        memcpy(t->exec_path, parent->exec_path, MAX_PATH_LEN);
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
        t->root_path[0] = '/'; t->root_path[1] = '\0';
        t->exec_path[0] = '\0';
    }
    if (t->cwd[0] == '\0') {
        t->cwd[0] = '/';
        t->cwd[1] = '\0';
    }
    if (t->root_path[0] == '\0') {
        t->root_path[0] = '/';
        t->root_path[1] = '\0';
    }

    // 处理文件描述符
    for (int i = 0; i < MAX_FILES; i++) {
        t->fd_table[i] = -1;
        t->fd_cloexec[i] = 0;
    }
    if (parent) {
        vfs_proc_copy_fds(parent->fd_table, t->fd_table);
        memcpy(t->fd_cloexec, parent->fd_cloexec, sizeof(t->fd_cloexec));
    }
    vfs_proc_init_stdio_defaults(t->fd_table);
    t->fd_cloexec[0] = 0;
    t->fd_cloexec[1] = 0;
    t->fd_cloexec[2] = 0;

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
    t->pid = proc_alloc_pid();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    init_task_common(t, current_task);
    proc_register_pid(t);
    vfs_proc_close_all_fds(t->fd_table);
    memset(t->fd_cloexec, 0, sizeof(t->fd_cloexec));
    vfs_proc_init_stdio_defaults(t->fd_table);
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->root_path[0] = '/';
    t->root_path[1] = '\0';
    proc_set_name(t, "kthread");

    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (!stack) {
        proc_destroy_task(t);
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
    proc_make_ready(t);
    return t->pid;
}

/* Allocate a user-mode task with given entry point and stack */
// 分配一个用户态任务
int proc_alloc_user_image(uint64_t entry, uint64_t sp, uint64_t *pgdir,
                          vm_area_t *mmap, uint64_t brk,
                          uint64_t stack_top, size_t total_vm) {
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_alloc_pid();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    init_task_common(t, current_task);
    proc_register_pid(t);
    t->entry = entry;
    t->pgdir = pgdir;
    proc_set_name(t, "user");

    void *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) {
        proc_destroy_task(t);
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
    arch_trap_ctx_set_kernel_stack(trap, ks_top);

    t->trap_ctx = trap;
    t->ustack   = sp;
    t->pgdir = pgdir;

    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (mm) {
        mm->pgdir       = pgdir;
        mm->brk         = brk;
        mm->start_brk   = brk;
        mm->mmap_base   = MMAP_BASE_ADDR;
        mm->stack_top   = stack_top ? stack_top : sp;
        mm->stack_bottom = mm->stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
        mm->total_vm    = total_vm;
        mm->rss         = 0;
        mm->refcount    = 1;
        mm->mmap        = mmap;
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

    proc_make_ready(t);
    return t->pid;
}

int proc_alloc_user(uint64_t entry, uint64_t sp, uint64_t *pgdir) {
    return proc_alloc_user_image(entry, sp, pgdir, NULL, 0, sp, 0);
}

/* ============================================================
 * Clone (fork)
 * ============================================================ */

// 克隆当前进程；支持 fork 路径和 pthread 常用的 CLONE_VM/TLS/TID 语义。
int proc_clone(uint64_t flags, uint64_t stack, int *ptid, uint64_t tls, int *ctid) {
    task_t *parent = current_task;
    task_t *t = alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_alloc_pid();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    init_task_common(t, parent);
    proc_register_pid(t);
    if ((flags & CLONE_PARENT) && parent && parent->parent) {
        t->parent = parent->parent;
        t->ppid = parent->parent->pid;
    }
    if (flags & CLONE_THREAD)
        t->tgid = parent ? parent->tgid : t->pid;
    t->clone_flags = (int)flags;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid;
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
        if (parent->mm && (flags & CLONE_VM)) {
            t->mm = parent->mm;
            t->mm->refcount++;
            t->pgdir = parent->pgdir;
        } else if (parent->mm) {
            t->mm = mm_fork(parent->mm);
            if (!t->mm) {
                proc_destroy_task(t);
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
        proc_destroy_task(t);
        return -ENOMEM;
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
        arch_trap_ctx_set_kernel_stack(trap, ks_top);
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;
        if (stack) TRAP_CTX_SP(trap) = stack;
        if (flags & CLONE_SETTLS) TRAP_CTX_TP(trap) = tls;

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

    if ((flags & CLONE_PARENT_SETTID) && ptid) {
        int child_tid = t->pid;
        if (copy_to_user(ptid, &child_tid, sizeof(child_tid)) < 0) {
            proc_free_pid(t->pid);
            return -EFAULT;
        }
    }
    if ((flags & CLONE_CHILD_SETTID) && ctid) {
        int child_tid = t->pid;
        if (proc_copy_to_task_user(t, ctid, &child_tid, sizeof(child_tid)) < 0) {
            proc_free_pid(t->pid);
            return -EFAULT;
        }
    }

    proc_make_ready(t);
    if (flags & CLONE_VFORK) {
        while (t->state != PROC_UNUSED && t->state != PROC_ZOMBIE)
            proc_yield();
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

    kstat_t exec_st;
    int exec_stat_ok = vfs_stat(path, &exec_st);
    if (exec_stat_ok == 0 && (exec_st.st_mode & S_IFMT) == S_IFREG &&
        !(exec_st.st_mode & 0111)) {
        char magic[2];
        int n = vfs_read(fd, magic, sizeof(magic));
        vfs_lseek(fd, 0, SEEK_SET);
        if (n < 2 || magic[0] != '#' || magic[1] != '!') {
            vfs_close(fd);
            return -EACCES;
        }
    }

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

    char *k_argv[MAX_ARG_STRINGS + 1] = {0};
    char *k_envp[MAX_ARG_STRINGS + 1] = {0};
    int argc = 0, envc = 0;
    size_t arg_bytes = 0;
    int arg_error = 0;

    auto int is_kptr(const void *p) {
        return arch_is_kernel_address(p);
    }

    int argv_is_kernel = argv && is_kptr(argv);
    if (argv) {
        while (argc < MAX_ARG_STRINGS) {
            char *arg;
            if (argv_is_kernel) {
                arg = argv[argc];
            } else {
                if (copy_from_user(&arg, &argv[argc], sizeof(char*)) < 0) { arg_error = -EFAULT; break; }
            }
            if (!arg) break;
            k_argv[argc] = kmalloc(MAX_ARG_STRLEN);
            if (!k_argv[argc]) { arg_error = -ENOMEM; break; }
            if (argv_is_kernel) {
                size_t len = strlen(arg) + 1;
                if (len > MAX_ARG_STRLEN) {
                    kfree(k_argv[argc]); k_argv[argc] = NULL; arg_error = -E2BIG; break;
                }
                memcpy(k_argv[argc], arg, len);
                arg_bytes += len;
            } else {
                long copied = user_strncpy(k_argv[argc], arg, MAX_ARG_STRLEN);
                if (copied < 0) {
                    kfree(k_argv[argc]); k_argv[argc] = NULL; arg_error = (int)copied; break;
                }
                if ((size_t)copied >= MAX_ARG_STRLEN - 1) {
                    kfree(k_argv[argc]); k_argv[argc] = NULL; arg_error = -E2BIG; break;
                }
                arg_bytes += (size_t)copied + 1;
            }
            if (arg_bytes > MAX_ARG_BYTES || arg_bytes > (t ? t->rlim_stack / 4 : MAX_ARG_BYTES)) {
                kfree(k_argv[argc]); k_argv[argc] = NULL; arg_error = -E2BIG; break;
            }
            argc++;
        }
        if (!arg_error && argc == MAX_ARG_STRINGS) {
            char *extra;
            if (argv_is_kernel) extra = argv[argc];
            else if (copy_from_user(&extra, &argv[argc], sizeof(char *)) < 0) extra = NULL;
            if (extra) arg_error = -E2BIG;
        }
    }
    int envp_is_kernel = envp && is_kptr(envp);
    if (!arg_error && envp) {
        while (envc < MAX_ARG_STRINGS) {
            char *env;
            if (envp_is_kernel) {
                env = envp[envc];
            } else {
                if (copy_from_user(&env, &envp[envc], sizeof(char*)) < 0) { arg_error = -EFAULT; break; }
            }
            if (!env) break;
            k_envp[envc] = kmalloc(MAX_ARG_STRLEN);
            if (!k_envp[envc]) { arg_error = -ENOMEM; break; }
            if (envp_is_kernel) {
                size_t len = strlen(env) + 1;
                if (len > MAX_ARG_STRLEN) {
                    kfree(k_envp[envc]); k_envp[envc] = NULL; arg_error = -E2BIG; break;
                }
                memcpy(k_envp[envc], env, len);
                arg_bytes += len;
            } else {
                long copied = user_strncpy(k_envp[envc], env, MAX_ARG_STRLEN);
                if (copied < 0) {
                    kfree(k_envp[envc]); k_envp[envc] = NULL; arg_error = (int)copied; break;
                }
                if ((size_t)copied >= MAX_ARG_STRLEN - 1) {
                    kfree(k_envp[envc]); k_envp[envc] = NULL; arg_error = -E2BIG; break;
                }
                arg_bytes += (size_t)copied + 1;
            }
            if (arg_bytes > MAX_ARG_BYTES || arg_bytes > (t ? t->rlim_stack / 4 : MAX_ARG_BYTES)) {
                kfree(k_envp[envc]); k_envp[envc] = NULL; arg_error = -E2BIG; break;
            }
            envc++;
        }
        if (!arg_error && envc == MAX_ARG_STRINGS) {
            char *extra;
            if (envp_is_kernel) extra = envp[envc];
            else if (copy_from_user(&extra, &envp[envc], sizeof(char *)) < 0) extra = NULL;
            if (extra) arg_error = -E2BIG;
        }
    }
    if (arg_error) {
        vfs_close(fd);
        for (int i = 0; i < argc; i++) kfree(k_argv[i]);
        for (int i = 0; i < envc; i++) kfree(k_envp[i]);
        kfree(k_path);
        return arg_error;
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

    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_cloexec[i] && t->fd_table[i] >= 0) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
        t->fd_cloexec[i] = 0;
    }
    vfs_proc_init_stdio_defaults(t->fd_table);
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
    mm->stack_bottom = info.stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
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

    uint64_t saved_kernel_sp = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;

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
        saved_kernel_sp = ks_top;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        saved_kernel_sp = arch_trap_ctx_get_kernel_stack(trap, saved_kernel_sp);
        memset(trap, 0, sizeof(*trap));
        TRAP_CTX_KScratch0(trap) = arch_make_satp(info.pgdir);
        TRAP_CTX_EPC(trap)      = saved_entry;
        TRAP_CTX_SP(trap)      = saved_sp;
        TRAP_CTX_TP(trap)      = info.tls_tp;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, saved_kernel_sp);
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
static void proc_destroy_task(task_t *t) {
    if (!t) return;

    vfs_release_process_locks(t->pid);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    runq_remove_locked(t);
    pid_hash_remove(t);
    spin_unlock_irqrestore(&proc_lock, flags);

    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] >= 0) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
        t->fd_cloexec[i] = 0;
    }

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

void proc_free_pid(int pid) {
    task_t *t = proc_find(pid);
    if (!t) return;
    proc_destroy_task(t);
}

static void proc_reparent_children(task_t *dead, task_t *reaper) {
    if (!dead) return;
    if (!reaper || reaper == dead) reaper = &proc_table[0];
    int wake_reaper = 0;
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *child = &proc_table[i];
        if (child->state == PROC_UNUSED || child->ppid != dead->pid)
            continue;
        if (reaper == &proc_table[0] && child->state == PROC_ZOMBIE) {
            proc_destroy_task(child);
            continue;
        }
        child->ppid = reaper->pid;
        child->parent = reaper;
        if (child->state == PROC_ZOMBIE)
            wake_reaper = 1;
    }
    if (wake_reaper && reaper->state == PROC_BLOCKED)
        proc_make_ready(reaper);
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
    if (t->clear_child_tid) {
        int zero = 0;
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_user(t->clear_child_tid, 1);
        t->clear_child_tid = NULL;
    }
    vfs_release_process_locks(t->pid);
    /* Close all file descriptors */
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] >= 0) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
        t->fd_cloexec[i] = 0;
    }

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    {
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        runq_remove_locked(t);
        spin_unlock_irqrestore(&proc_lock, flags);
    }
    t->state     = PROC_ZOMBIE;

    /* Wake up parent if blocked in wait */
    if (t->parent && t->parent->state == PROC_BLOCKED) {
        proc_make_ready(t->parent);
    }

    /* Send SIGCHLD unless the parent asked POSIX-style auto-reap behavior. */
    if (t->parent && !proc_ignores_sigchld(t->parent))
        signal_send(t->parent->pid, SIGCHLD);

    sched();
    panic("proc_exit: sched returned");
}

void proc_force_exit(task_t *t, int exit_code) {
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;
    if (t == current_task)
        proc_exit(exit_code);

    if (t->clear_child_tid) {
        int zero = 0;
        task_t *saved = current_task;
        current_task = t;
        if (copy_to_user(t->clear_child_tid, &zero, sizeof(zero)) == 0)
            futex_wake_user(t->clear_child_tid, 1);
        current_task = saved;
        t->clear_child_tid = NULL;
    }

    vfs_release_process_locks(t->pid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] >= 0) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
        t->fd_cloexec[i] = 0;
    }

    task_t *reaper = proc_find(1);
    proc_reparent_children(t, reaper);

    t->exit_code = exit_code;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    runq_remove_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);
    t->state = PROC_ZOMBIE;

    if (t->parent && t->parent->state == PROC_BLOCKED)
        proc_make_ready(t->parent);
    if (t->parent && !proc_ignores_sigchld(t->parent))
        signal_send(t->parent->pid, SIGCHLD);
}

void proc_exit_group(int exit_code) {
    task_t *self = current_task;
    if (!self)
        proc_exit(exit_code);

    mm_struct_t *mm = self->mm;
    for (int i = 1; i < MAX_PROCS; i++) {
        task_t *t = &proc_table[i];
        if (t == self || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
            continue;
        if (mm && t->mm == mm)
            proc_force_exit(t, exit_code);
    }
    proc_exit(exit_code);
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
                    *status = (-code) & 0xFF;
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
                    *status = (-code) & 0xFF;
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
    if (!next || !next->kstack)
        return;
    if (next == current_task) {
        next->state = PROC_RUNNING;
        next->on_rq = 0;
        return;
    }
    task_t *prev = current_task;
    current_task = next;
    next->state  = PROC_RUNNING;
    next->on_rq  = 0;
    if (prev)
        arch_set_task_pointer(prev);
    __switch(next->kstack);
}

// 调度器：选择下一个运行的任务
void sched(void) {
    uint64_t now = timer_get_ticks();

    virtio_blk_poll_all();
    a20_lwip_poll();

    /* Wake up sleeping processes */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED && proc_table[i].alarm_expire > 0
            && now >= proc_table[i].alarm_expire) {
            uint64_t interval = proc_table[i].itimer_real_interval;
            proc_table[i].alarm_expire = interval ? now + interval : 0;
            signal_send(proc_table[i].pid, SIGALRM);
        }
        if (proc_table[i].state == PROC_BLOCKED && proc_table[i].wake_time > 0
            && now >= proc_table[i].wake_time) {
            if (sched_diag_count < 128) {
                sched_diag_count++;
                kdebug("[SCHEDDBG] wake pid=%d now=%lu wake=%lu\n",
                      proc_table[i].pid,
                      (unsigned long)now,
                      (unsigned long)proc_table[i].wake_time);
            }
            proc_table[i].wake_time = 0;
            proc_table[i].sched_level = 0;
            proc_make_ready(&proc_table[i]);
        }
    }

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *next = runq_pick_locked();
    spin_unlock_irqrestore(&proc_lock, flags);

    if (next) {
        if (sched_diag_count < 256) {
            sched_diag_count++;
            kdebug("[SCHEDDBG] pick cur=%d next=%d level=%d state=%d\n",
                  current_task ? current_task->pid : -1, next->pid,
                  next->sched_level, next->state);
        }
        context_switch(next);
        return;
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
        current_task->state == PROC_RUNNING) {
        if (current_task->sched_level < SCHED_LEVELS - 1)
            current_task->sched_level++;
        current_task->state = PROC_READY;
        proc_make_ready(current_task);
    }
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
    arch_tlb_flush();
    return mapped;
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
