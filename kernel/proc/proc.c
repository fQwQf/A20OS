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
#include "core/progress.h"

static task_t idle_tasks[CONFIG_NR_CPUS];
static task_t *task_list_head;
static task_t *task_list_tail;
static pt_root_t *kernel_pgdir_shared;

spinlock_t proc_lock = SPINLOCK_INIT;

static uint64_t g_idle_kstack[CONFIG_NR_CPUS];
ARCH_IDLE_CONTEXT_STATIC(arch_idle_context, CONFIG_NR_CPUS);

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
    task_t *next = t ? t->all_next : NULL;
    if (next && (((uintptr_t)next & (sizeof(void *) - 1)) ||
                 !arch_is_kernel_address(next))) {
        kerr("proc_next_task_locked: corrupt all_next from pid=%d ptr=%p\n",
             t ? t->pid : -1, (void *)next);
        return NULL;
    }
    return next;
}

static void proc_count_vma_huge_pages(mm_struct_t *mm, vm_area_t *vma,
                                      proc_vm_stats_t *stats)
{
    if (!mm || !mm->pgdir || !vma || !stats)
        return;

    for (uint64_t va = vma->start; va < vma->end; ) {
        mm_leaf_info_t leaf;
        if (mm_query_leaf(mm->pgdir, va, &leaf)) {
            if (leaf.level > 0) {
                size_t pages = leaf.size / PAGE_SIZE;
                if ((vma->vm_flags & VM_ANON) && (vma->vm_flags & VM_SHARED))
                    stats->shmem_huge_pages += pages;
                else if (vma->vm_flags & VM_ANON)
                    stats->anon_huge_pages += pages;
                else
                    stats->file_huge_pages += pages;
            }
            va = leaf.base + leaf.size;
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

    mm_struct_t *seen_mm[256];
    int seen_count = 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || !t->mm)
            continue;

        int duplicate = 0;
        for (int i = 0; i < seen_count; i++) {
            if (seen_mm[i] == t->mm) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        if (seen_count < (int)(sizeof(seen_mm) / sizeof(seen_mm[0])))
            seen_mm[seen_count++] = t->mm;

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

void proc_sleep_until(uint64_t wake_time) {
    (void)proc_park_wait(PROC_WAIT_UNINTERRUPTIBLE, wake_time);
}

// idle 进程的主循环，系统无任务时运行
void idle_loop(void) {
    while (1) {
        arch_local_irq_enable();
        kernel_progress_run_bottom_halves();
        sched();
        cpu_relax();
    }
}

// 初始化进程管理模块，创建 idle 进程
void proc_init(void) {
    memset(idle_tasks, 0, sizeof(idle_tasks));
    task_list_head = NULL;
    task_list_tail = NULL;
    proc_pid_init();
    proc_sched_runq_init();
    spin_init(&proc_lock);

    task_t *idle = &idle_tasks[0];
    proc_link_task_locked(idle);
    idle->pid    = 0;
    idle->ppid   = 0;
    proc_task_init_idle_state(idle, 0);
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
    idle->cpus_allowed = CONFIG_NR_CPUS >= 32
                         ? ~0U : (1U << CONFIG_NR_CPUS) - 1U;
    proc_set_name(idle, "idle");
    proc_pid_register(idle);

#ifndef CONFIG_MCU
    fdtable_init(idle);
#endif
    idle->parent  = NULL;

    /* Allocate signal state */
#ifndef CONFIG_MCU
    idle->signals = (struct signal_state *)kmalloc(sizeof(signal_state_t));
    if (idle->signals) signal_init((signal_state_t *)idle->signals);
#endif

    // 分配内核栈
    void *idle_stack = ARCH_IDLE_STACK(arch_idle_context, 0);
    if (!idle_stack) panic("proc_init: no memory for idle stack");
    ARCH_IDLE_STACK_INIT(idle_stack);
    uintptr_t stack_top = ARCH_IDLE_STACK_TOP(idle_stack);
    task_context_t *ctx = arch_task_context_base(idle_stack, stack_top, NULL);
    memset(ctx, 0, sizeof(*ctx));
    idle->first_kernel_entry = (uintptr_t)idle_loop;
    ctx->ra   = (uintptr_t)proc_task_first_entry;
    ctx->tp   = (uintptr_t)idle;
    arch_task_context_set_initial_sp(ctx, NULL, stack_top);

    // 创建并映射内核页表
    pt_root_t *kpdir = pt_create();
    if (!kpdir) panic("proc_init: pt_create failed");
    pt_map_kernel(kpdir);
    kernel_pgdir_shared = kpdir;
    idle->pgdir = kpdir;
    TASK_CTX_PAGE_TABLE(ctx) = kpdir ? arch_make_addr_space_token(kpdir) : 0;
    TASK_CTX_STATUS(ctx) = arch_task_kernel_status();
    idle->kstack_base = idle_stack;
    idle->kstack = (uintptr_t)ctx;
    g_idle_kstack[0] = idle->kstack;

    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++) {
        task_t *secondary = &idle_tasks[cpu];
        secondary->pid = 0;
        proc_task_init_idle_state(secondary, cpu);
        secondary->sched_level = SCHED_LEVELS - 1;
        secondary->cpus_allowed = 1U << cpu;
        secondary->pgdir = kpdir;
        proc_set_name(secondary, "idle");

        void *stack = ARCH_IDLE_STACK(arch_idle_context, cpu);
        if (!stack)
            panic("proc_init: no memory for secondary idle stack");
        ARCH_IDLE_STACK_INIT(stack);
        uintptr_t top = ARCH_IDLE_STACK_TOP(stack);
        task_context_t *secondary_ctx = arch_task_context_base(stack, top, NULL);
        memset(secondary_ctx, 0, sizeof(*secondary_ctx));
        secondary->first_kernel_entry = (uintptr_t)idle_loop;
        secondary_ctx->ra = (uintptr_t)proc_task_first_entry;
        secondary_ctx->tp = (uintptr_t)secondary;
        TASK_CTX_PAGE_TABLE(secondary_ctx) = arch_make_addr_space_token(kpdir);
        TASK_CTX_STATUS(secondary_ctx) = arch_task_kernel_status();
        secondary->kstack_base = stack;
        secondary->kstack = (uintptr_t)secondary_ctx;
        g_idle_kstack[cpu] = secondary->kstack;
    }

    arch_set_task_pointer(idle);  // 设置 tp 寄存器
    proc_set_current(idle);

    kdebug("[PROC] Initialized, idle task pid=0\n");
}

void proc_init_secondary(unsigned cpu_id)
{
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        panic("proc_init_secondary: invalid cpu %u", cpu_id);

    task_t *idle = &idle_tasks[cpu_id];
    arch_set_task_pointer(idle);
    proc_set_current(idle);
}

task_t *proc_idle_task(void) { return &idle_tasks[cpu_current_id()]; }

pt_root_t *proc_kernel_pgdir_shared(void) { return kernel_pgdir_shared; }

/* ---- Base task allocation ---- */

// 分配一个空闲的任务槽
task_t *proc_alloc_task_slot(void) {
    task_t *t = proc_task_alloc_storage();
    if (!t)
        return NULL;

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
#ifndef CONFIG_MCU
    fdtable_close_all(t);
    fdtable_init_stdio(t);
#endif
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

    uintptr_t stack_top = (uintptr_t)stack + KERNEL_STACK_SIZE;

    task_context_t *ctx = arch_task_context_base(stack, stack_top, NULL);
    memset(ctx, 0, sizeof(*ctx));
    t->first_kernel_entry = (uintptr_t)entry;
    ctx->ra   = (uintptr_t)proc_task_first_entry;
    ctx->tp   = (uintptr_t)t;
    t->pgdir  = kernel_pgdir_shared;
    TASK_CTX_PAGE_TABLE(ctx) = kernel_pgdir_shared ? arch_make_addr_space_token(kernel_pgdir_shared) : 0;
    TASK_CTX_STATUS(ctx) = arch_task_kernel_status();
    arch_task_context_set_initial_sp(ctx, NULL, stack_top);
    t->kstack_base = stack;
    t->kstack = (uintptr_t)ctx;

    kdebug("[PROC] kthread pid=%d\n", t->pid);
    proc_make_ready(t);
    return t->pid;
}

/* Allocate a user-mode task with given entry point and stack */
// 分配一个用户态任务
int proc_alloc_user_image(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir,
                          vm_area_t *mmap, vaddr_t brk,
                          vaddr_t stack_top, size_t total_vm,
                          vaddr_t tls_tp
#ifdef CONFIG_NOMMU
                          , void **nommu_allocs, const size_t *nommu_alloc_sizes,
                          const uint8_t *nommu_alloc_types, int num_nommu_allocs
#endif
                          ) {
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

    uintptr_t ks_top = (uintptr_t)kstack + KERNEL_STACK_SIZE;
    ks_top &= ~0xF;

    trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
    memset(trap, 0, sizeof(*trap));
    arch_trap_ctx_set_user_entry(trap, entry);
    TRAP_CTX_SP(trap)   = sp;
    TRAP_CTX_TP(trap)    = tls_tp;
    TRAP_CTX_STATUS(trap) = arch_user_initial_status();
    TRAP_CTX_KScratch0(trap) = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    trap->kernel_tp = (uintptr_t)t;
    arch_trap_ctx_set_kernel_stack(trap, (uint64_t)ks_top);

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
        spin_init(&mm->lock);
        spin_set_debug(&mm->lock, "mm", mm);
        refcount_set(&mm->refcount, 1);
        mm->mmap        = mmap;
#ifdef CONFIG_NOMMU
        if (nommu_allocs && num_nommu_allocs > 0) {
            mm->num_nommu_allocs = num_nommu_allocs < NOMMU_ALLOC_MAX ?
                num_nommu_allocs : NOMMU_ALLOC_MAX;
            for (int i = 0; i < mm->num_nommu_allocs; i++) {
                mm->nommu_allocs[i] = nommu_allocs[i];
                mm->nommu_alloc_sizes[i] = nommu_alloc_sizes ?
                    nommu_alloc_sizes[i] : 0;
                mm->nommu_alloc_types[i] = nommu_alloc_types ?
                    nommu_alloc_types[i] : NOMMU_ALLOC_IMAGE;
            }
        }
#endif
        ktrace_mm("[MMDBG] mm=%p lock=%p\n", (void *)mm, (void *)&mm->lock);
        t->mm = mm;
    }

    /*
     * Ask the architecture where the initial task_context_t belongs.  Most
     * arches place it just below the pre-allocated trap frame; x86_64 places
     * it at the bottom of the kernel stack so the C call stack cannot
     * overwrite it.
     */
    task_context_t *ctx = arch_task_context_base(kstack, ks_top, trap);
    memset(ctx, 0, sizeof(*ctx));
    t->first_kernel_entry = (uintptr_t)user_trap_return;
    ctx->ra   = (uintptr_t)proc_task_first_entry;
    ctx->tp   = (uintptr_t)t;
    arch_task_context_set_user_tp(ctx, tls_tp);
    TASK_CTX_PAGE_TABLE(ctx) = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    TASK_CTX_STATUS(ctx) = arch_task_user_resume_status();
    arch_task_context_set_initial_sp(ctx, trap, ks_top);
    t->kstack = (uintptr_t)ctx;

    kinfo("[PROC] user task pid=%d entry=0x%lx sp=0x%lx trap_sp=0x%lx\n", t->pid,
          (unsigned long)entry, (unsigned long)sp, (unsigned long)TRAP_CTX_SP(trap));

    proc_make_ready(t);
    return t->pid;
}

int proc_alloc_user(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir) {
    return proc_alloc_user_image(entry, sp, pgdir, NULL, 0, sp, 0, 0
#ifdef CONFIG_NOMMU
                               , NULL, NULL, NULL, 0
#endif
                               );
}

/* ============================================================
 * Kill
 * ============================================================ */

// 向指定进程发送信号（kill 系统调用的实现）
int proc_kill(int pid, int signum) {
    return signal_send_user(pid, signum);
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
            signal_send_user(pids[i], signum);
        count += pid_count;
    }
    return count > 0 ? count : -ESRCH;
}

/* ============================================================
 * mmap / brk
 * ============================================================ */

// 调整堆大小（brk 系统调用的实现）
vaddr_t proc_brk(vaddr_t newbrk) {
    task_t *t = proc_current();
    if (!t || !t->mm) return 0; // 理论上不应发生

    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);

    // 如果 newbrk 为 0，通常是 C 库在查询当前堆位置
    if (newbrk == 0) {
        vaddr_t brk = t->mm->brk;
        spin_unlock_irqrestore(&t->mm->lock, lock_flags);
        return brk;
    }

    // mm_brk 内部已经处理了 newbrk < start_brk 的情况（返回旧 brk）
    // 同时也处理了分配失败的情况
    vaddr_t brk = mm_brk(t->mm, newbrk);
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    return brk;
}

// 创建内存映射（mmap 系统调用的实现）
vaddr_t proc_mmap(vaddr_t addr, size_t len, int prot, int flags, int fd, long off) {
    task_t *t = proc_current();
    if (!t || !t->mm) return (uint64_t)-1;

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) return (uint64_t)-EINVAL;

    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);
    vaddr_t ret;
    if ((flags & MAP_ANONYMOUS) || fd < 0)
        ret = mm_mmap(t->mm, addr, len, prot, flags);
    else {
        if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1))) {
            spin_unlock_irqrestore(&t->mm->lock, lock_flags);
            return (uint64_t)-EINVAL;
        }

        ret = mm_mmap_file(t->mm, addr, len, prot, flags, fd, (uint64_t)off);
    }
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    return ret;
}

// 取消内存映射（munmap 系统调用的实现）
int proc_munmap(vaddr_t addr, size_t len) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -1;
    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);
    int ret = mm_munmap(t->mm, addr, len);
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    return ret;
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
