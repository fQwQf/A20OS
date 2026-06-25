#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/progress.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/trap.h"
#include "fs/fdtable.h"
#include "mm/mm.h"
#include "mm/vm.h"

static task_t g_proc_core_idle_tasks[CONFIG_NR_CPUS];
static uint64_t *g_proc_core_kernel_pgdir_shared;

extern spinlock_t proc_lock;

unsigned a20_proc_core_current_cpu_id(void) { return cpu_current_id(); }
void a20_proc_core_arch_local_irq_enable(void) { arch_local_irq_enable(); }
void a20_proc_core_spin_init(spinlock_t *lock) { spin_init(lock); }
void a20_proc_core_panic(const char *msg) { panic("%s", msg); }

task_t *a20_proc_core_idle_task_slot(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        cpu = 0;
    return &g_proc_core_idle_tasks[cpu];
}

uint64_t *a20_proc_core_kernel_pgdir_shared_get(void) { return g_proc_core_kernel_pgdir_shared; }
void a20_proc_core_kernel_pgdir_shared_set(uint64_t *pgdir) { g_proc_core_kernel_pgdir_shared = pgdir; }

void a20_proc_core_zero_task(task_t *task)
{
    if (task)
        memset(task, 0, sizeof(*task));
}

void a20_proc_core_init_idle_task_fields(task_t *idle)
{
    idle->pid = 0;
    idle->ppid = 0;
    idle->state = PROC_RUNNING;
    idle->fs.cwd[0] = '/';
    idle->fs.cwd[1] = '\0';
    idle->fs.root_path[0] = '/';
    idle->fs.root_path[1] = '\0';
    idle->pgid = 0;
    idle->sid = 0;
    idle->fs.umask = 022;
    idle->cred.uid = 0;
    idle->cred.euid = 0;
    idle->cred.suid = 0;
    idle->cred.fsuid = 0;
    idle->cred.gid = 0;
    idle->cred.egid = 0;
    idle->cred.sgid = 0;
    idle->cred.fsgid = 0;
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
}

task_t *a20_proc_core_task_alloc_zero(void)
{
    return kcalloc(1, sizeof(task_t));
}

void a20_proc_core_task_set_blocked_dynamic(task_t *task)
{
    if (!task)
        return;
    task->state = PROC_BLOCKED;
    task->dynamic_alloc = 1;
}

void a20_proc_core_task_set_entry_pgdir(task_t *task, uint64_t entry, uint64_t *pgdir)
{
    if (!task)
        return;
    task->entry = entry;
    task->pgdir = pgdir;
}

int a20_proc_core_task_state(task_t *task) { return task ? (int)task->state : PROC_UNUSED; }
void a20_proc_core_task_set_state(task_t *task, int state) { if (task) task->state = (proc_state_t)state; }
int a20_proc_core_task_pid(task_t *task) { return task ? task->pid : -1; }
int a20_proc_core_task_ppid(task_t *task) { return task ? task->ppid : -1; }
int a20_proc_core_task_pgid(task_t *task) { return task ? task->pgid : -1; }
unsigned a20_proc_core_task_cpu_id(task_t *task) { return task ? task->cpu_id : 0; }
void a20_proc_core_task_set_cpu_id(task_t *task, unsigned cpu) { if (task) task->cpu_id = cpu; }
int a20_proc_core_task_on_rq(task_t *task) { return task ? task->on_rq : 0; }
int a20_proc_core_task_vfork_waiting(task_t *task) { return task ? task->vfork_waiting : 0; }
void a20_proc_core_task_set_wake_time(task_t *task, uint64_t wake_time) { if (task) task->wake_time = wake_time; }
uint64_t a20_proc_core_task_wake_time(task_t *task) { return task ? task->wake_time : 0; }
int a20_proc_core_task_sched_level(task_t *task) { return task ? task->sched_level : 0; }
void a20_proc_core_task_set_sched_level(task_t *task, int level) { if (task) task->sched_level = level; }
int a20_proc_core_task_priority(task_t *task) { return task ? task->priority : 0; }
const char *a20_proc_core_task_name(task_t *task) { return task ? task->name : ""; }
mm_struct_t *a20_proc_core_task_mm(task_t *task) { return task ? task->mm : NULL; }

void a20_proc_core_task_alloc_signals(task_t *task)
{
    if (!task)
        return;
    task->signals = kmalloc(sizeof(signal_state_t));
    if (task->signals)
        signal_init((signal_state_t *)task->signals);
}

void *a20_proc_core_alloc_zero_kstack(void)
{
    void *stack = kmalloc(KERNEL_STACK_SIZE);
    if (stack)
        memset(stack, 0, KERNEL_STACK_SIZE);
    return stack;
}

uint64_t *a20_proc_core_create_kernel_pgdir(void)
{
    uint64_t *kpdir = pt_create();
    if (kpdir)
        pt_map_kernel(kpdir);
    return kpdir;
}

int a20_proc_core_setup_idle_context(task_t *task, void *stack, uint64_t *pgdir, void (*entry)(void))
{
    if (!task || !stack || !pgdir)
        return -1;
    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;
    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra = (uint64_t)entry;
    ctx->tp = (uint64_t)task;
    task->pgdir = pgdir;
    TASK_CTX_PAGE_TABLE(ctx) = arch_make_addr_space_token(pgdir);
    TASK_CTX_STATUS(ctx) = arch_task_kernel_status();
    task->kstack_base = stack;
    task->kstack = (uint64_t)ctx;
    return 0;
}

void a20_proc_core_activate_idle(task_t *task)
{
    arch_set_task_pointer(task);
    proc_set_current(task);
}

int a20_proc_core_setup_kthread_context(task_t *task, void *stack, uint64_t *pgdir, void (*entry)(void))
{
    if (!task || !stack)
        return -1;
    uint64_t stack_top = (uint64_t)stack + KERNEL_STACK_SIZE;
    task_context_t *ctx = (task_context_t *)(stack_top - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra = (uint64_t)entry;
    ctx->tp = (uint64_t)task;
    task->pgdir = pgdir;
    TASK_CTX_PAGE_TABLE(ctx) = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    TASK_CTX_STATUS(ctx) = arch_task_kernel_status();
    task->kstack_base = stack;
    task->kstack = (uint64_t)ctx;
    return 0;
}

int a20_proc_core_setup_user_context(task_t *task, void *stack, uint64_t entry, uint64_t sp, uint64_t *pgdir)
{
    if (!task || !stack)
        return -1;
    uint64_t ks_top = (uint64_t)stack + KERNEL_STACK_SIZE;
    trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
    memset(trap, 0, sizeof(*trap));
    TRAP_CTX_EPC(trap) = entry;
    TRAP_CTX_SP(trap) = sp;
    TRAP_CTX_STATUS(trap) = arch_user_initial_status();
    TRAP_CTX_KScratch0(trap) = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    trap->kernel_tp = (uint64_t)(uintptr_t)task;
    arch_trap_ctx_set_kernel_stack(trap, ks_top);
    task->trap_ctx = trap;
    task->ustack = sp;
    task->pgdir = pgdir;
    task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
    memset(ctx, 0, sizeof(*ctx));
    ctx->ra = (uint64_t)user_trap_return;
    ctx->tp = (uint64_t)task;
    TASK_CTX_PAGE_TABLE(ctx) = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    TASK_CTX_STATUS(ctx) = arch_user_initial_status();
    task->kstack_base = stack;
    task->kstack = (uint64_t)ctx;
    return 0;
}

mm_struct_t *a20_proc_core_create_user_mm(uint64_t *pgdir, vm_area_t *mmap, uint64_t brk,
                                          uint64_t stack_top, uint64_t sp, size_t total_vm)
{
    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm)
        return NULL;
    mm->pgdir = pgdir;
    mm->brk = brk;
    mm->start_brk = brk;
    mm->mmap_base = MMAP_BASE_ADDR;
    mm->stack_top = stack_top ? stack_top : sp;
    mm->stack_bottom = mm->stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    mm->total_vm = total_vm;
    mm->rss = 0;
    spin_init(&mm->lock);
    spin_set_debug(&mm->lock, "mm", mm);
    refcount_set(&mm->refcount, 1);
    mm->mmap = mmap;
    return mm;
}

vm_area_t *a20_proc_core_mm_first_vma(mm_struct_t *mm) { return mm ? mm->mmap : NULL; }
vm_area_t *a20_proc_core_vma_next(vm_area_t *vma) { return vma ? vma->next : NULL; }

void a20_proc_core_count_vma_huge_pages(mm_struct_t *mm, vm_area_t *vma, proc_vm_stats_t *stats)
{
    if (!mm || !mm->pgdir || !vma || !stats)
        return;
    for (uint64_t va = vma->start; va < vma->end;) {
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

size_t a20_proc_core_pidmap_write_header(char *buf, size_t bufsz, int pid_max, int next_pid, int used)
{
    int n = snprintf(buf, bufsz, "pid_max: %d\nnext_pid: %d\nused: %d\npids:", pid_max, next_pid, used);
    if (n <= 0)
        return 0;
    return (size_t)n >= bufsz ? bufsz - 1 : (size_t)n;
}

size_t a20_proc_core_pidmap_append_pid(char *buf, size_t bufsz, size_t off, int pid)
{
    int n = snprintf(buf + off, bufsz - off, " %d", pid);
    if (n <= 0)
        return off;
    return (size_t)n >= bufsz - off ? bufsz - 1 : off + (size_t)n;
}

size_t a20_proc_core_pidmap_finish(char *buf, size_t bufsz, size_t off)
{
    if (off + 1 < bufsz)
        buf[off++] = '\n';
    buf[off < bufsz ? off : bufsz - 1] = '\0';
    return off;
}

void a20_proc_core_dump_task_line(int pid, int ppid, int state, int priority, const char *name)
{
    const char *s = "?";
    switch (state) {
    case PROC_READY: s = "RDY"; break;
    case PROC_RUNNING: s = "RUN"; break;
    case PROC_BLOCKED: s = "BLK"; break;
    case PROC_ZOMBIE: s = "ZOM"; break;
    default: break;
    }
    printf("  %3d   %3d   %s   %3d  %s\n", pid, ppid, s, priority, name ? name : "");
}

uint64_t a20_proc_core_proc_brk(task_t *task, uint64_t newbrk)
{
    if (!task || !task->mm)
        return 0;
    uint64_t flags = spin_lock_irqsave(&task->mm->lock);
    if (newbrk == 0) {
        uint64_t brk = task->mm->brk;
        spin_unlock_irqrestore(&task->mm->lock, flags);
        return brk;
    }
    uint64_t brk = mm_brk(task->mm, newbrk);
    spin_unlock_irqrestore(&task->mm->lock, flags);
    return brk;
}

uint64_t a20_proc_core_proc_mmap(task_t *task, uint64_t addr, size_t len, int prot, int flags, int fd, unsigned long off)
{
    if (!task || !task->mm)
        return (uint64_t)-1;
    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0)
        return (uint64_t)-EINVAL;
    uint64_t lock_flags = spin_lock_irqsave(&task->mm->lock);
    uint64_t ret;
    if ((flags & MAP_ANONYMOUS) || fd < 0) {
        ret = mm_mmap(task->mm, addr, len, prot, flags);
    } else {
        if ((off & (PAGE_SIZE - 1)) != 0) {
            spin_unlock_irqrestore(&task->mm->lock, lock_flags);
            return (uint64_t)-EINVAL;
        }
        ret = mm_mmap_file(task->mm, addr, len, prot, flags, fd, off);
    }
    spin_unlock_irqrestore(&task->mm->lock, lock_flags);
    return ret;
}

int a20_proc_core_proc_munmap(task_t *task, uint64_t addr, size_t len)
{
    if (!task || !task->mm)
        return -1;
    uint64_t lock_flags = spin_lock_irqsave(&task->mm->lock);
    int ret = mm_munmap(task->mm, addr, len);
    spin_unlock_irqrestore(&task->mm->lock, lock_flags);
    return ret;
}
