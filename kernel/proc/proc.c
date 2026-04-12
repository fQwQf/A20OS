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
#include "trap.h"
#include "timer.h"
#include "stdio.h"
#include "string.h"
#include "panic.h"
#include "consts.h"
#include "defs.h"
#include "klog.h"
#include "arch_ops.h"

static task_t proc_table[MAX_PROCS];
static uint64_t *kernel_pgdir_shared;
static task_t *current_task = NULL;

/* Boot stack (for schedule back to idle) */
static uint64_t g_idle_kstack;

void idle_loop(void) {
    while (1) {
        arch_cpu_wait();
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

    for (int i = 0; i < MAX_FILES; i++) idle->fd_table[i] = -1;
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
    if (kpdir) pt_map_kernel(kpdir);
    kernel_pgdir_shared = kpdir;
    idle->pgdir = kpdir;
    ctx->satp = arch_mmu_token_from_pgdir(kpdir);
    ctx->sstatus = arch_task_status_kernel_default();
    idle->kstack_base = idle_stack;
    idle->kstack = (uint64_t)ctx;
    g_idle_kstack = idle->kstack;

    arch_set_current_task_ptr(idle);
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
    t->brk       = 0;
    t->mmap_base = 0x40000000UL; /* 1GB base for mmap */
    t->vm_areas  = NULL;

    if (parent) {
        memcpy(t->cwd, parent->cwd, MAX_PATH_LEN);
    } else {
        t->cwd[0] = '/'; t->cwd[1] = '\0';
    }

    for (int i = 0; i < MAX_FILES; i++) t->fd_table[i] = -1;
    /* stdin/stdout/stderr from parent or defaults */
    t->fd_table[0] = 0;
    t->fd_table[1] = 1;
    t->fd_table[2] = 2;
    if (parent) {
        for (int i = 3; i < MAX_FILES; i++) t->fd_table[i] = parent->fd_table[i];
    }

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
    if (!stack) { t->state = PROC_UNUSED; return -ENOMEM; }
    memset(stack, 0, KERNEL_STACK_SIZE);

    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;

    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra   = (uint64_t)entry;
    ctx->tp   = (uint64_t)t;
    t->pgdir  = kernel_pgdir_shared;
    ctx->satp = arch_mmu_token_from_pgdir(kernel_pgdir_shared);
    ctx->sstatus = arch_task_status_kernel_default();
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
    if (!kstack) { t->state = PROC_UNUSED; return -ENOMEM; }
    memset(kstack, 0, KERNEL_STACK_SIZE);
    t->kstack_base = kstack;

    uint64_t ks_top = (uint64_t)kstack + KERNEL_STACK_SIZE;

    trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
    memset(trap, 0, sizeof(*trap));
    trap->sepc   = entry;
    trap->x[0]   = arch_mmu_token_from_pgdir(pgdir);
    trap->x[2]   = sp;
    trap->sstatus = 0;

    t->trap_ctx = trap;
    t->ustack   = sp;

    task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    extern void user_trap_return(void);
    ctx->ra   = (uint64_t)user_trap_return;
    ctx->tp   = (uint64_t)t;
    ctx->satp = arch_mmu_token_from_pgdir(pgdir);
    ctx->sstatus = arch_task_status_kernel_default();
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
    t->brk            = parent->brk;
    t->mmap_base      = parent->mmap_base;

    if (parent->pgdir) {
        t->pgdir = pt_clone(parent->pgdir);
        if (!t->pgdir) { t->state = PROC_UNUSED; return -ENOMEM; }
    }

    void *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack) { pt_destroy_user(t->pgdir); t->state = PROC_UNUSED; return -ENOMEM; }
    memset(kstack, 0, KERNEL_STACK_SIZE);
    t->kstack_base = kstack;

    uint64_t ks_top = (uint64_t)kstack + KERNEL_STACK_SIZE;

    if (parent->trap_ctx) {
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        *trap = *parent->trap_ctx;
        trap->x[0]   = arch_mmu_token_from_pgdir(t->pgdir);
        trap->x[10] = 0;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        extern void user_trap_return(void);
        ctx->ra   = (uint64_t)user_trap_return;
        ctx->tp   = (uint64_t)t;
        ctx->satp = arch_mmu_token_from_pgdir(t->pgdir);
        t->kstack = (uint64_t)ctx;
    } else {
        task_context_t *ctx = (task_context_t *)(ks_top - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra   = (uint64_t)idle_loop;
        ctx->tp   = (uint64_t)t;
        t->kstack = (uint64_t)ctx;
    }

    kdebug("[PROC] clone: parent=%d child=%d pgdir=0x%lx\n",
          parent->pid, t->pid, (unsigned long)(uintptr_t)t->pgdir);
    return t->pid;
}

/* ============================================================
 * Exec — replace process image with ELF
 * ============================================================ */

int proc_exec(const char *path, char *const argv[], char *const envp[]) {
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) { printf("[EXEC] Cannot open %s: %d\n", path, fd); return fd; }

    task_t *t = current_task;

    char *k_path = kmalloc(strlen(path) + 1);
    strcpy(k_path, path);

    char *k_argv[32] = {0};
    char *k_envp[32] = {0};
    int argc = 0, envc = 0;
    
    if (argv) {
        while (argv[argc] && argc < 31) {
            k_argv[argc] = kmalloc(strlen(argv[argc]) + 1);
            strcpy(k_argv[argc], argv[argc]);
            argc++;
        }
    }
    if (envp) {
        while (envp[envc] && envc < 31) {
            k_envp[envc] = kmalloc(strlen(envp[envc]) + 1);
            strcpy(k_envp[envc], envp[envc]);
            envc++;
        }
    }

    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    int r = elf_load(fd, &info);
    vfs_close(fd);
    if (r < 0) {
        printf("[EXEC] ELF load failed: %d\n", r);
        for(int i=0; i<argc; i++) kfree(k_argv[i]);
        for(int i=0; i<envc; i++) kfree(k_envp[i]);
        kfree(k_path);
        return r;
    }
    kdebug("[EXEC] ELF loaded: entry=0x%lx base=0x%lx brk=0x%lx stack=0x%lx\n",
           (unsigned long)info.entry, (unsigned long)info.base,
           (unsigned long)info.brk, (unsigned long)info.stack_top);

    uint64_t sp = elf_setup_stack(info.stack_top, argc,
                                   (char *const *)k_argv,
                                   (char *const *)k_envp, &info);

    for(int i=0; i<argc; i++) kfree(k_argv[i]);
    for(int i=0; i<envc; i++) kfree(k_envp[i]);

    uint64_t *old_pgdir = t->pgdir;
    t->pgdir = info.pgdir;
    t->entry = info.entry;
    t->ustack = sp;
    t->brk   = info.brk;
    proc_set_name(t, path);

    if (t->signals) signal_init((signal_state_t *)t->signals);

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
        ctx->satp = arch_mmu_token_from_pgdir(info.pgdir);
        t->trap_ctx = trap;
        t->kstack   = (uint64_t)ctx;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        memset(trap, 0, sizeof(*trap));
        trap->sepc      = saved_entry;
        trap->x[0]      = arch_mmu_token_from_pgdir(info.pgdir);
        trap->x[2]      = saved_sp;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        trap->sstatus   = 0x0UL;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        ctx->satp = arch_mmu_token_from_pgdir(info.pgdir);
    }

    t->exec_load_addr = info.load_addr;
    t->exec_load_size = info.load_size;

    pt_destroy_user(old_pgdir);

    kdebug("[EXEC] Executing %s pid=%d entry=0x%lx pgdir=0x%lx\n", k_path, t->pid,
           (unsigned long)saved_entry, (unsigned long)(uintptr_t)info.pgdir);

    kfree(k_path);

    arch_sync_icache();

    t->state = PROC_RUNNING;
    return 0;
}

/* ============================================================
 * Free / Exit
 * ============================================================ */

void proc_free_pid(int pid) {
    task_t *t = proc_find(pid);
    if (!t) return;

    pt_destroy_user(t->pgdir);
    t->pgdir = NULL;

    if (t->signals) { kfree(t->signals); t->signals = NULL; }

    vm_area_t *va = t->vm_areas;
    while (va) {
        vm_area_t *next = va->next;
        kfree(va);
        va = next;
    }

    if (t->kstack) {
        uintptr_t sp = (uintptr_t)t->kstack;
        void *stack_base = (void *)(sp & ~((uintptr_t)KERNEL_STACK_SIZE - 1));
        kfree(stack_base);
        t->kstack = 0;
    }

    memset(t, 0, sizeof(*t));
}

void proc_exit(int exit_code) {
    task_t *t = current_task;
    if (!t) panic("proc_exit: no current task");

    kdebug("[PROC] pid=%d exit_code=%d\n", t->pid, exit_code);

    /* Close all file descriptors */
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] > 2) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = -1;
        }
    }

    /* Re-parent children to idle */
    for (int i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].ppid == t->pid && proc_table[i].state != PROC_UNUSED) {
            proc_table[i].ppid   = 0;
            proc_table[i].parent = &proc_table[0];
        }
    }

    /* Wake up parent if blocked in wait */
    if (t->parent && t->parent->state == PROC_BLOCKED) {
        t->parent->state = PROC_READY;
    }

    /* Send SIGCHLD to parent */
    if (t->parent) signal_send(t->parent->pid, SIGCHLD);

    t->state     = PROC_ZOMBIE;
    t->exit_code = exit_code;

    kdebug("[PROC] pid=%d calling sched()...\n", t->pid);
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
        if (child->state == PROC_UNUSED) continue;
        if (child->ppid != t->pid) continue;
        if (pid > 0 && child->pid != pid) continue;
        if (pid <= -1 && pid != -1 && child->pgid != (-pid)) continue;

        found = 1;
        if (child->state == PROC_ZOMBIE) {
            if (status) *status = (child->exit_code & 0xFF) << 8;
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

    // kinfo("[SCHED] cur_idx=%d cur_pid=%d cur_state=%d\n",
    //      cur_idx, current_task ? current_task->pid : -1,
    //      current_task ? current_task->state : -1);

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
            kdebug("[SCHED] Found ready task: pid=%d idx=%d\n", t->pid, idx);
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
    kdebug("[SCHED] No ready tasks found, switching to idle\n");
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
    if (!t) return -1;
    if (newbrk == 0) return t->brk;
    if (newbrk < t->brk) return t->brk;

    uint64_t old_brk = ROUND_UP(t->brk, PAGE_SIZE);
    uint64_t target  = ROUND_UP(newbrk, PAGE_SIZE);

    if (t->pgdir) {
        for (uint64_t va = old_brk; va < target; va += PAGE_SIZE) {
            if (pt_translate(t->pgdir, va) == 0) {
                void *frame = frame_alloc();
                if (!frame) { printf("[BRK] OOM at va=0x%lx\n", (unsigned long)va); return t->brk; }
                memset(frame, 0, PAGE_SIZE);
                pt_map(t->pgdir, va, (paddr_t)(uintptr_t)frame,
                        PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);
            }
        }
    }

    t->brk = newbrk;
    return t->brk;
}

uint64_t proc_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    (void)fd; (void)off;
    task_t *t = current_task;
    if (!t) return (uint64_t)-1;

    len = ROUND_UP(len, PAGE_SIZE);

    uint64_t va = addr ? addr : t->mmap_base;
    if (!addr) t->mmap_base += len;

    if (t->pgdir) {
        for (uint64_t off2 = 0; off2 < len; off2 += PAGE_SIZE) {
            void *frame = frame_alloc();
            if (!frame) return (uint64_t)-ENOMEM;
            memset(frame, 0, PAGE_SIZE);
            uint64_t pte_flags = PTE_V | PTE_U | PTE_A | PTE_D;
            if (prot & 1) pte_flags |= PTE_R;
            if (prot & 2) pte_flags |= PTE_W;
            if (prot & 4) pte_flags |= PTE_X;
            pt_map(t->pgdir, va + off2, (paddr_t)(uintptr_t)frame, pte_flags);
        }
    }

    vm_area_t *va_node = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (!va_node) return (uint64_t)-ENOMEM;
    va_node->start = va;
    va_node->end   = va + len;
    va_node->prot  = prot;
    va_node->flags = flags;
    va_node->next  = t->vm_areas;
    t->vm_areas    = va_node;

    return va;
}

int proc_munmap(uint64_t addr, size_t len) {
    (void)len;
    task_t *t = current_task;
    if (!t) return -1;

    vm_area_t **pp = &t->vm_areas;
    while (*pp) {
        if ((*pp)->start == addr) {
            vm_area_t *doomed = *pp;
            *pp = doomed->next;
            if (t->pgdir) {
                for (uint64_t va = doomed->start; va < doomed->end; va += PAGE_SIZE) {
                    paddr_t pa = pt_translate(t->pgdir, va);
                    pt_unmap(t->pgdir, va);
                    if (pa) frame_free((void *)(uintptr_t)pa);
                }
            }
            kfree(doomed);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return 0;
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
