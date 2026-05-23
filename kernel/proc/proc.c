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
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "mm/elf.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "core/cpu.h"
#include "core/trap.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "sys/futex.h"
#include "drv/virtio_blk.h"
#include "net/lwip_stack.h"

static task_t idle_task;
static task_t *task_list_head;
static task_t *task_list_tail;
static uint64_t *kernel_pgdir_shared;  // 共享的内核页表

spinlock_t proc_lock = SPINLOCK_INIT;

/* Boot stack (for schedule back to idle) */
static uint64_t g_idle_kstack;  // idle 进程的内核栈

static void proc_link_task_locked(task_t *t)
{
    t->all_prev = task_list_tail;
    t->all_next = NULL;
    if (task_list_tail)
        task_list_tail->all_next = t;
    else
        task_list_head = t;
    task_list_tail = t;
}

void proc_unlink_task_locked(task_t *t)
{
    if (!t)
        return;
    if (t->all_prev)
        t->all_prev->all_next = t->all_next;
    else if (task_list_head == t)
        task_list_head = t->all_next;
    if (t->all_next)
        t->all_next->all_prev = t->all_prev;
    else if (task_list_tail == t)
        task_list_tail = t->all_prev;
    t->all_next = NULL;
    t->all_prev = NULL;
}

task_t *proc_first_task_locked(void)
{
    return task_list_head;
}

task_t *proc_next_task_locked(task_t *t)
{
    return t ? t->all_next : NULL;
}

static void proc_count_vma_huge_pages(mm_struct_t *mm, vm_area_t *vma,
                                      proc_vm_stats_t *stats)
{
    if (!mm || !mm->pgdir || !vma || !stats)
        return;

    for (uint64_t va = vma->start; va < vma->end; ) {
        int level = 0;
        uint64_t base = 0;
        size_t size = 0;
        uint64_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte)) {
            if (level > 0) {
                size_t pages = size / PAGE_SIZE;
                if ((vma->vm_flags & VM_ANON) && (vma->vm_flags & VM_SHARED))
                    stats->shmem_huge_pages += pages;
                else if (vma->vm_flags & VM_ANON)
                    stats->anon_huge_pages += pages;
                else
                    stats->file_huge_pages += pages;
            }
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
}

void proc_get_vm_stats(proc_vm_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || !t->mm)
            continue;

        int duplicate = 0;
        for (task_t *prev = proc_first_task_locked(); prev && prev != t;
             prev = proc_next_task_locked(prev)) {
            if (prev->state != PROC_UNUSED && prev->mm == t->mm) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        for (vm_area_t *v = t->mm->mmap; v; v = v->next)
            proc_count_vma_huge_pages(t->mm, v, stats);
    }

    spin_unlock_irqrestore(&proc_lock, flags);
}

size_t proc_format_pidmap(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    size_t off = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int used = 0;

    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state != PROC_UNUSED)
            used++;
    }

    int n = snprintf(buf + off, bufsz - off,
                     "pid_max: %d\nnext_pid: %d\nused: %d\npids:",
                     proc_pid_max(), proc_pid_next_value(), used);
    if (n > 0) {
        size_t wrote = (size_t)n;
        off = wrote >= bufsz - off ? bufsz - 1 : off + wrote;
    }

    for (task_t *t = proc_first_task_locked(); t && off + 16 < bufsz;
         t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;
        n = snprintf(buf + off, bufsz - off, " %d", t->pid);
        if (n <= 0)
            break;
        size_t wrote = (size_t)n;
        off = wrote >= bufsz - off ? bufsz - 1 : off + wrote;
    }
    if (off + 1 < bufsz)
        buf[off++] = '\n';
    buf[off < bufsz ? off : bufsz - 1] = '\0';

    spin_unlock_irqrestore(&proc_lock, flags);
    return off;
}

static int proc_parent_auto_reaps_children(task_t *parent)
{
    if (!parent || parent->state == PROC_UNUSED || !parent->signals)
        return 0;

    signal_state_t *ss = (signal_state_t *)parent->signals;
    sigaction_t *act = &ss->actions[SIGCHLD];
    return act->sa_handler == SIG_IGN || (act->sa_flags & SA_NOCLDWAIT);
}

static int proc_zombie_is_detached(task_t *t)
{
    if (!t)
        return 0;
    if (t->ppid == 0 || t->parent == proc_idle_task())
        return 1;
    if (t->clone_flags & CLONE_THREAD)
        return 1;
    return t->exit_signal == SIGCHLD &&
           proc_parent_auto_reaps_children(t->parent);
}

static void proc_reap_detached_zombies(void) {
    for (;;) {
        int target_pid = -1;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == proc_idle_task())
                continue;
            if (t->state == PROC_ZOMBIE && proc_zombie_is_detached(t)) {
                target_pid = t->pid;
                break;
            }
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        if (target_pid < 0)
            break;
        task_t *t = proc_find(target_pid);
        if (t)
            proc_destroy_task(t);
    }
}

void proc_make_ready(task_t *t) {
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
        return;

    unsigned target_cpu = cpu_current_id();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state != PROC_READY) {
        t->state = PROC_READY;
        if (t->wake_time == 0 && t->sched_level > 0)
            t->sched_level--;
    }
    if (!t->on_rq) {
        if (t == proc_current())
            t->cpu_id = cpu_current_id();
        else
            t->cpu_id = proc_sched_select_cpu_locked(t);
    }
    target_cpu = t->cpu_id;
    spin_unlock_irqrestore(&proc_lock, flags);

    proc_runq_enqueue_locked(t);

    if (target_cpu != cpu_current_id())
        proc_sched_kick_cpu(target_cpu);
}

void proc_block_until(task_t *t, uint64_t wake_time) {
    if (!t)
        return;
    if (wake_time)
        proc_set_wake_time(t, wake_time);
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    t->state = PROC_BLOCKED;
    spin_unlock_irqrestore(&proc_lock, flags);
}

// idle 进程的主循环，系统无任务时运行
void idle_loop(void) {
    while (1) {
        arch_local_irq_enable();
        virtio_blk_poll_all();
        a20_lwip_poll();
        sched_reap_zombies();
        sched();
        __asm__ volatile("nop");
    }
}

// 初始化进程管理模块，创建 idle 进程
void proc_init(void) {
    memset(&idle_task, 0, sizeof(idle_task));
    task_list_head = NULL;
    task_list_tail = NULL;
    proc_pid_init();
    proc_sched_runq_init();
    spin_init(&proc_lock);

    task_t *idle = &idle_task;
    proc_link_task_locked(idle);
    idle->pid    = 0;
    idle->ppid   = 0;
    idle->state  = PROC_RUNNING;
    idle->fs.cwd[0] = '/';
    idle->fs.cwd[1] = '\0';
    idle->fs.root_path[0] = '/';
    idle->fs.root_path[1] = '\0';
    idle->pgid   = 0;
    idle->sid    = 0;
    idle->fs.umask  = 022;
    idle->cred.uid    = 0;
    idle->cred.euid   = 0;
    idle->cred.suid   = 0;
    idle->cred.fsuid  = 0;
    idle->cred.gid    = 0;
    idle->cred.egid   = 0;
    idle->cred.sgid   = 0;
    idle->cred.fsgid  = 0;
    idle->cred.ngroups = 0;
    idle->cred.cap_effective = ~(uint64_t)0;
    idle->cred.cap_permitted = ~(uint64_t)0;
    idle->cred.cap_inheritable = 0;
    idle->cred.cap_bounding = ~(uint64_t)0;
    idle->policy.oom_score_adj = 0;
    idle->policy.thp_disabled = 0;
    idle->limits.stack = USER_STACK_MAX_SIZE;
    idle->limits.nofile = MAX_FILES;
    idle->limits.memlock = 64 * 1024;
    idle->sched_level = SCHED_LEVELS - 1;
    idle->cpu_id = 0;
    proc_set_name(idle, "idle");
    proc_pid_register(idle);

    fdtable_init(idle);
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
    proc_set_current(idle);

    kdebug("[PROC] Initialized, idle task pid=0\n");
}

task_t *proc_idle_task(void) { return &idle_task; }

uint64_t *proc_kernel_pgdir_shared(void) { return kernel_pgdir_shared; }

/* ---- Base task allocation ---- */

// 分配一个空闲的任务槽
task_t *proc_alloc_task_slot(void) {
    task_t *t = kcalloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->state = PROC_BLOCKED;
    t->dynamic_alloc = 1;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_link_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);
    return t;
}

// 分配一个内核线程
int proc_alloc(void (*entry)(void)) {
    task_t *t = proc_alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    proc_task_init_common(t, proc_current());
    proc_pid_register(t);
    fdtable_close_all(t);
    fdtable_init_stdio(t);
    t->fs.cwd[0] = '/';
    t->fs.cwd[1] = '\0';
    t->fs.root_path[0] = '/';
    t->fs.root_path[1] = '\0';
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
    task_t *t = proc_alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    proc_task_init_common(t, proc_current());
    proc_pid_register(t);
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
        refcount_set(&mm->refcount, 1);
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
 * Kill
 * ============================================================ */

// 向指定进程发送信号（kill 系统调用的实现）
int proc_kill(int pid, int signum) {
    return signal_send(pid, signum);
}

int proc_kill_pgid(int pgid, int signum, int skip_self) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *self = proc_current();
    int count = 0;
    int pids[64];

    for (;;) {
        int pid_count = 0;
        int seen = 0;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == proc_idle_task()) continue;
            if (t->state == PROC_UNUSED) continue;
            if (t->pgid != pgid) continue;
            if (skip_self && t == self) continue;
            if (seen++ < count) continue;
            pids[pid_count++] = t->pid;
            if (pid_count == (int)(sizeof(pids) / sizeof(pids[0])))
                break;
        }
        spin_unlock_irqrestore(&proc_lock, flags);

        if (pid_count == 0)
            break;
        for (int i = 0; i < pid_count; i++)
            signal_send(pids[i], signum);
        count += pid_count;
    }
    return count > 0 ? count : -ESRCH;
}

/* ============================================================
 * mmap / brk
 * ============================================================ */

// 调整堆大小（brk 系统调用的实现）
uint64_t proc_brk(uint64_t newbrk) {
    task_t *t = proc_current();
    if (!t || !t->mm) return 0; // 理论上不应发生

    // 如果 newbrk 为 0，通常是 C 库在查询当前堆位置
    if (newbrk == 0) return t->mm->brk;

    // mm_brk 内部已经处理了 newbrk < start_brk 的情况（返回旧 brk）
    // 同时也处理了分配失败的情况
    return mm_brk(t->mm, newbrk);
}

// 创建内存映射（mmap 系统调用的实现）
uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    task_t *t = proc_current();
    if (!t || !t->mm) return (uint64_t)-1;

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) return (uint64_t)-EINVAL;

    if ((flags & MAP_ANONYMOUS) || fd < 0)
        return mm_mmap(t->mm, addr, len, prot, flags);

    if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1)))
        return (uint64_t)-EINVAL;

    return mm_mmap_file(t->mm, addr, len, prot, flags, fd, (uint64_t)off);
}

// 取消内存映射（munmap 系统调用的实现）
int proc_munmap(uint64_t addr, size_t len) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -1;
    return mm_munmap(t->mm, addr, len);
}

// 打印所有进程信息
void proc_dump(void) {
    printf("  PID  PPID  STATE  PRI  NAME\n");
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED) continue;
        const char *s = "?";
        switch (t->state) {
            case PROC_READY:   s = "RDY"; break;
            case PROC_RUNNING: s = "RUN"; break;
            case PROC_BLOCKED: s = "BLK"; break;
            case PROC_ZOMBIE:  s = "ZOM"; break;
            default: break;
        }
        printf("  %3d   %3d   %s   %3d  %s\n",
               t->pid, t->ppid, s, t->priority, t->name);
    }
    spin_unlock_irqrestore(&proc_lock, flags);
}
