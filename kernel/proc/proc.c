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
#include "drivers/core/driver_core.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "core/cpu.h"
#include "core/lock_counters.h"
#include "core/perf.h"
#include "core/trap.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timer.h"
#include "core/lock.h"
#include "sys/futex.h"

static task_t idle_tasks[CONFIG_NR_CPUS];
static task_t *task_list_head;
static task_t *task_list_tail;
static pt_root_t *kernel_pgdir_shared;

/*
 * tasklist_lock is the only remaining global lock in the proc subsystem.  It
 * replaces the former proc_lock, whose single critical section covered three
 * unrelated domains at once; this lock covers exactly one of them:
 *
 *   - global task-list membership (task_list_head/tail, ->all_next, ->all_prev)
 *   - parent/children/sibling links and the thread-group chain
 *
 * It deliberately does NOT protect scheduling state (->state, ->on_cpu,
 * ->exit_pending, ...) nor CPU ownership (INV-P5).  Those live on the owning
 * task's park_lock, which is why the context-switch publication path no
 * longer touches any global lock.  Order: tasklist_lock -> park_lock.
 */
spinlock_t tasklist_lock = SPINLOCK_INIT;

#if CONFIG_DEBUG_SCHED_STATE
/* Hang-diagnostic task snapshot (see idle_loop).  Static because MCU kernel
 * stacks are 512-2048 bytes and the dump runs on the idle task.  Guarded by
 * the same CONFIG_DEBUG_SCHED_STATE as its only user (the dump block in
 * proc_idle_loop): without the guard a non-DEBUG build compiles the array out
 * of every use and -Werror=unused-variable fails the build. */
enum { HANG_SNAP_MAX = 32 };
static struct {
    int pid;
    int state;
    int on_cpu;
    int on_rq;
    unsigned owner_cpu;
    int park;
    char name[16];
} g_hang_snap[HANG_SNAP_MAX];
#endif /* CONFIG_DEBUG_SCHED_STATE */

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

/* Link a freshly created (non-fork) task into the wait4 children-list
 * model: fork() does this inline, but the a20 task_spawn/exec creation
 * paths (proc_alloc, proc_alloc_user_image) historically relied on the
 * old global-list wait4 scan and never linked -- breaking both wait4
 * (-ECHILD on live children) and orphan reparenting under the list
 * model.  Takes and releases tasklist_lock internally. */
void proc_link_newborn(task_t *t)
{
    if (!t)
        return;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    if (!t->tg_leader)
        t->tg_leader = t;
    proc_children_link_locked(t->parent, t);
    spin_unlock_irqrestore(&tasklist_lock, flags);
}

void proc_children_link_locked(task_t *parent, task_t *child)
{
    if (!parent || !child || child->sibling_prev_ptr)
        return;
    child->sibling_next = parent->children;
    child->sibling_prev_ptr = &parent->children;
    if (child->sibling_next)
        child->sibling_next->sibling_prev_ptr = &child->sibling_next;
    parent->children = child;
}

void proc_children_unlink_locked(task_t *t)
{
    if (!t || !t->sibling_prev_ptr)
        return;
    *t->sibling_prev_ptr = t->sibling_next;
    if (t->sibling_next)
        t->sibling_next->sibling_prev_ptr = t->sibling_prev_ptr;
    t->sibling_next = NULL;
    t->sibling_prev_ptr = NULL;
}

void proc_reparent_task_locked(task_t *new_parent, task_t *t)
{
    if (!t || !new_parent)
        return;
    proc_children_unlink_locked(t);
    t->parent = new_parent;
    t->ppid = new_parent->pid;
    proc_children_link_locked(new_parent, t);
}

void proc_tg_link_locked(task_t *t)
{
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    if (t == leader || leader == t->tg_next || t->tg_prev_ptr)
        return;
    t->tg_next = leader->tg_next;
    t->tg_prev_ptr = &leader->tg_next;
    if (t->tg_next)
        t->tg_next->tg_prev_ptr = &t->tg_next;
    leader->tg_next = t;
}

void proc_tg_unlink_locked(task_t *t)
{
    if (!t || !t->tg_prev_ptr)
        return;
    *t->tg_prev_ptr = t->tg_next;
    if (t->tg_next)
        t->tg_next->tg_prev_ptr = t->tg_prev_ptr;
    t->tg_next = NULL;
    t->tg_prev_ptr = NULL;
}

void proc_unlink_task_locked(task_t *t)
{
    if (!t)
        return;
    proc_children_unlink_locked(t);
    proc_tg_unlink_locked(t);
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

/*
 * INV-P3: when two different tasks' park_locks must be held at the same time,
 * they are taken in ascending task-pointer order and released in reverse.
 * Every site that nests two task locks goes through this pair, so the order is
 * defined in exactly one place.  Per-CPU slots (g_cpu_switch_out[cpu]) sit
 * outside this order and never take part in it.
 */
void proc_lock_two_tasks(task_t *a, task_t *b, uint64_t *flags_a,
                                 uint64_t *flags_b)
{
    *flags_a = 0;
    *flags_b = 0;
    if (!a)
        return;
    if (!b || a == b) {
        *flags_a = spin_lock_irqsave(&a->park_lock);
        return;
    }
    if ((uintptr_t)a < (uintptr_t)b) {
        *flags_a = spin_lock_irqsave(&a->park_lock);
        *flags_b = spin_lock_irqsave(&b->park_lock);
    } else {
        *flags_b = spin_lock_irqsave(&b->park_lock);
        *flags_a = spin_lock_irqsave(&a->park_lock);
    }
}

void proc_unlock_two_tasks(task_t *a, task_t *b, uint64_t flags_a,
                                   uint64_t flags_b)
{
    if (!a)
        return;
    if (!b || a == b) {
        spin_unlock_irqrestore(&a->park_lock, flags_a);
        return;
    }
    if ((uintptr_t)a < (uintptr_t)b) {
        spin_unlock_irqrestore(&b->park_lock, flags_b);
        spin_unlock_irqrestore(&a->park_lock, flags_a);
    } else {
        spin_unlock_irqrestore(&a->park_lock, flags_a);
        spin_unlock_irqrestore(&b->park_lock, flags_b);
    }
}

/*
 * The runq-owned half of the snapshot cannot be taken under the task's
 * park_lock, because the pick side publishes ->dispatching/->owner_cpu under
 * the runqueue lock alone and must never take park_lock (INV-P4b).  Those four
 * fields are therefore read as relaxed atomics: the snapshot is a diagnostic /
 * low-frequency-iteration view, not a linearization point against a concurrent
 * pick.  The two fields the caller most often branches on (state, on_cpu) are
 * park_lock-owned and are read consistently.
 *
 * The result is built in a local value and handed to the caller with a single
 * struct assignment, never through per-field stores into *out.  Reason: the
 * check-task-state-boundary gate asserts that no arrow store of a PROC_ state,
 * and no arrow store to on_rq / dispatching / on_cpu / owner_cpu / rq_next /
 * rq_prev, appears anywhere outside the park-lock owning files, because that
 * is exactly the shape a lockless write of a task field has.  A per-field store
 * into a caller-owned struct that is not a task_t is indistinguishable from
 * that shape to that check; filling a local value with '.' members keeps the
 * assertion exactly as it was, still in force over every real task_t store in
 * the tree, instead of widening its file whitelist to exempt this helper.
 */
void proc_task_sched_state_snapshot(task_t *t, proc_task_sched_state_t *out)
{
    if (!out)
        return;
    proc_task_sched_state_t snap;
    memset(&snap, 0, sizeof(snap));
    if (!t) {
        snap.state = PROC_UNUSED;
        snap.owner_cpu = PROC_CPU_NONE;
        *out = snap;
        return;
    }

    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    snap.state = t->state;
    snap.on_cpu = t->on_cpu;
    spin_unlock_irqrestore(&t->park_lock, flags);

    snap.on_rq = __atomic_load_n(&t->on_rq, __ATOMIC_RELAXED);
    snap.dispatching = __atomic_load_n(&t->dispatching, __ATOMIC_RELAXED);
    snap.owner_cpu = __atomic_load_n(&t->owner_cpu, __ATOMIC_RELAXED);
    snap.cpu_id = __atomic_load_n(&t->cpu_id, __ATOMIC_RELAXED);
    *out = snap;
}

int proc_task_state_get(task_t *t)
{
    if (!t)
        return PROC_UNUSED;
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    int state = t->state;
    spin_unlock_irqrestore(&t->park_lock, flags);
    return state;
}

static void proc_count_vma_huge_pages(mm_struct_t *mm, mm_seg_t *vma,
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

    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED || !t->mm)
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

        for (mm_seg_t *v = t->mm->mmap; v; v = v->next)
            proc_count_vma_huge_pages(t->mm, v, stats);
    }

    spin_unlock_irqrestore(&tasklist_lock, flags);
}

size_t proc_format_pidmap(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    size_t off = 0;
    /* E1: only list membership is read here, so tasklist_lock suffices.  The
     * UNUSED test still needs the park-owned state word, hence the per-task
     * read helper instead of a direct ->state load. */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    int used = 0;

    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) != PROC_UNUSED)
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
        if (proc_task_state_get(t) == PROC_UNUSED)
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

    spin_unlock_irqrestore(&tasklist_lock, flags);
    return off;
}

void proc_sleep_until(uint64_t wake_time) {
    while (timer_get_ticks() < wake_time) {
        proc_wake_reason_t reason =
            proc_park_wait(PROC_WAIT_UNINTERRUPTIBLE, wake_time);
        if (reason != PROC_WAKE_TIMEOUT_CAPACITY)
            break;
        proc_yield();
    }
}

// The idle process's main loop, which runs when the system has no tasks
void idle_loop(void) {
    /* One query replaces four CONFIG_BOARD_LS2K1000 tests.  A board whose timer
     * cannot preempt needs the idle path to spin with interrupts enabled; every
     * other board sleeps. */
    const int spin_idle = current_board ? current_board->idle_cannot_sleep : 0;
    int first_schedule = 1;
#if CONFIG_DEBUG_SCHED_STATE
    uint64_t last_activity = timer_get_ticks();
    uint64_t last_warn = 0;
#endif
    while (1) {
        if (!spin_idle)
            arch_local_irq_enable();
        /* sched() drains bottom halves before selecting a task.  Calling the
         * same hook here doubled per-device progress callbacks on every idle
         * pass without opening an additional completion opportunity. */
        if (spin_idle && first_schedule)
            kinfo("[SCHED] spinning idle path, first schedule\n");
        sched();
        if (spin_idle && first_schedule) {
            kinfo("[SCHED] spinning idle path, first schedule returned\n");
            first_schedule = 0;
        }
#if ARCH_HAS_SAFE_IDLE_WAIT
        if (spin_idle) {
            cpu_relax();
        } else {
        arch_local_irq_disable();
        a20_perf_count(A20_PERF_IDLE_WAIT_ATTEMPTS);
        if (proc_sched_idle_prepare()) {
            a20_perf_count(A20_PERF_IDLE_WAIT_ENTRIES);
            arch_idle_wait();
            a20_perf_count(A20_PERF_IDLE_WAIT_WAKE_RETURNS);
        }
        arch_local_irq_enable();
        }
#else
        cpu_relax();
#endif
#if CONFIG_DEBUG_SCHED_STATE
        /* Hang diagnostic: if no non-idle task has run for a while, dump. */
        uint64_t now = timer_get_ticks();
        if (now - last_activity > 3 * TICKS_PER_SEC &&
            now - last_warn > 2 * TICKS_PER_SEC) {
            /* Snapshot under the locks, print with none held: the dump writes
             * through the console, and the idle path runs it with interrupts
             * disabled, so holding a global lock across the printf is the
             * self-deadlock the E3 rule exists to prevent.  The buffer is
             * file-scope, not stack: MCU kernel stacks are 512-2048 bytes and
             * this dump runs on the idle task.  Concurrent dumpers on two CPUs
             * may interleave their lines; that is acceptable for a hang
             * diagnostic and preferable to a stack overflow. */
            int snap_count = 0;
            int nonidle_running = 0;

            uint64_t flags = spin_lock_irqsave(&tasklist_lock);
            for (task_t *t = proc_first_task_locked(); t;
                 t = proc_next_task_locked(t)) {
                proc_task_sched_state_t st;
                proc_task_sched_state_snapshot(t, &st);
                if (t->pid != 0 && st.state == PROC_RUNNING && st.on_cpu) {
                    nonidle_running = 1;
                    break;
                }
                if (st.state == PROC_UNUSED)
                    continue;
                if (snap_count < HANG_SNAP_MAX) {
                    g_hang_snap[snap_count].pid = t->pid;
                    g_hang_snap[snap_count].state = (int)st.state;
                    g_hang_snap[snap_count].on_cpu = st.on_cpu;
                    g_hang_snap[snap_count].on_rq = st.on_rq;
                    g_hang_snap[snap_count].owner_cpu = st.owner_cpu;
                    g_hang_snap[snap_count].park =
                        (int)__atomic_load_n(&t->park_state, __ATOMIC_RELAXED);
                    strncpy(g_hang_snap[snap_count].name, t->name,
                            sizeof(g_hang_snap[snap_count].name) - 1);
                    g_hang_snap[snap_count].name[
                        sizeof(g_hang_snap[snap_count].name) - 1] = '\0';
                    snap_count++;
                }
            }
            spin_unlock_irqrestore(&tasklist_lock, flags);

            if (nonidle_running) {
                last_activity = now;
                continue;
            }
            last_warn = now;
            printf("[HANG] cpu=%u no progress for %lu ticks; tasks:\n",
                   cpu_current_id(),
                   (unsigned long)(now - last_activity));
            for (int i = 0; i < snap_count; i++) {
                printf("  pid=%d name=%s state=%d on_cpu=%d on_rq=%d cpu=%u park=%d\n",
                       g_hang_snap[i].pid, g_hang_snap[i].name,
                       g_hang_snap[i].state, g_hang_snap[i].on_cpu,
                       g_hang_snap[i].on_rq, g_hang_snap[i].owner_cpu,
                       g_hang_snap[i].park);
            }
            extern void a20_channel_trace_dump(void);
            a20_channel_trace_dump();
        }
#endif
    }
}

// Initialise the process management module and create the idle process
void proc_init(void) {
    memset(idle_tasks, 0, sizeof(idle_tasks));
    task_list_head = NULL;
    task_list_tail = NULL;
    proc_pid_init();
    pidns_early_init();
    userns_early_init();
    proc_sched_runq_init();
    proc_current_slots_init();
    spin_init(&tasklist_lock);
    spin_set_debug(&tasklist_lock, "tasklist", NULL);
    lock_counters_register(&tasklist_lock, "tasklist");
    lock_counters_enable_callsite(&tasklist_lock);

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
    idle->limits.as = 0;
    idle->limits.nproc = 0;
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

    arch_set_task_pointer(idle);  // set the tp register
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

task_t *proc_alloc_task_slot(void) {
    task_t *t = proc_task_alloc_storage();
    if (!t)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    proc_link_task_locked(t);
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return t;
}

// Allocate a kernel thread
int proc_alloc(void (*entry)(void)) {
    task_t *t = proc_alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    proc_task_init_common(t, proc_current(), 0);
    proc_link_newborn(t);
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
int proc_alloc_user_image(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir,
                          mm_seg_t *mmap, vaddr_t brk,
                          vaddr_t stack_top, size_t total_vm,
                          vaddr_t tls_tp
#ifdef CONFIG_NOMMU
                          , void **nommu_allocs, const size_t *nommu_alloc_sizes,
                          const uint8_t *nommu_alloc_types, int num_nommu_allocs
#endif
                          , int defer_ready) {
    task_t *t = proc_alloc_task_slot();
    if (!t) return -EAGAIN;
    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    proc_task_init_common(t, proc_current(), 0);
    proc_link_newborn(t);
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
    uint64_t user_as = pgdir ? arch_make_addr_space_token(pgdir) : 0;
    TRAP_CTX_KScratch0(trap) = user_as;
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
        mm->mmap_base   = mm_aslr_mmap_base();
        mm->stack_top   = stack_top ? stack_top : sp;
        mm->stack_bottom = mm->stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
        mm->total_vm    = total_vm;
        mm_rss_set(mm, 0);
        spin_init(&mm->lock);
        spin_set_debug(&mm->lock, "mm", mm);
        mutex_init(&mm->tlb_lock);
        mm->tlb_holds = NULL;
        mm_arch_context_init(mm);
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
        user_as = mm_address_space_token(mm);
        TRAP_CTX_KScratch0(trap) = user_as;
        /* This path bypasses exec(), which maps the signal return
         * trampoline in its own mm setup; without it, a handler return
         * jumps to an unmapped restorer address (x86_64 SIGSEGV). */
        arch_setup_signal_trampoline(mm);
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
    TASK_CTX_PAGE_TABLE(ctx) = user_as;
    TASK_CTX_STATUS(ctx) = arch_task_user_resume_status();
    arch_task_context_set_initial_sp(ctx, trap, ks_top);
    t->kstack = (uintptr_t)ctx;

    kinfo("[PROC] user task pid=%d entry=0x%lx sp=0x%lx trap_sp=0x%lx\n", t->pid,
          (unsigned long)entry, (unsigned long)sp, (unsigned long)TRAP_CTX_SP(trap));

    if (!defer_ready)
        proc_make_ready(t);
    return t->pid;
}

void proc_publish_deferred_task(task_t *task)
{
    proc_make_ready(task);
}

int proc_alloc_user(uintptr_t entry, vaddr_t sp, pt_root_t *pgdir) {
    return proc_alloc_user_image(entry, sp, pgdir, NULL, 0, sp, 0, 0
#ifdef CONFIG_NOMMU
                               , NULL, NULL, NULL, 0
#endif
                               , 0);
}

/* ============================================================
 * Kill
 * ============================================================ */

/* Console TIOCGPGRP self-heal: is any live user task in this process group? */
int proc_pgid_alive(int pgid) {
    if (pgid <= 0) return 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    int alive = 0;
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == proc_idle_task()) continue;
        int state = proc_task_state_get(t);
        if (state == PROC_UNUSED || state == PROC_ZOMBIE) continue;
        if (!t->pgdir) continue;
        if (t->pgid == pgid) { alive = 1; break; }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return alive;
}

// Send a signal to the given process; the implementation of the kill syscall
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
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == proc_idle_task()) continue;
            if (proc_task_state_get(t) == PROC_UNUSED) continue;
            if (t->pgid != pgid) continue;
            if (skip_self && t == self) continue;
            if (seen++ < count) continue;
            pids[pid_count++] = t->pid;
            if (pid_count == (int)(sizeof(pids) / sizeof(pids[0])))
                break;
        }
        spin_unlock_irqrestore(&tasklist_lock, flags);

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

// Adjust the heap size; the implementation of the brk syscall
vaddr_t proc_brk(vaddr_t newbrk) {
    task_t *t = proc_current();
    if (!t || !t->mm) return 0; // should not happen in theory

    if (newbrk != 0)
        mm_tlb_invalidate_begin(t->mm);
    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);

    // A newbrk of 0 usually means the C library is querying the current break
    if (newbrk == 0) {
        vaddr_t brk = t->mm->brk;
        spin_unlock_irqrestore(&t->mm->lock, lock_flags);
        return brk;
    }

    // mm_brk_locked: the caller already holds mm->lock
    vaddr_t brk = mm_brk_locked(t->mm, newbrk);
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    mm_tlb_invalidate_finish(t->mm);
    return brk;
}

// Create a memory mapping; the implementation of the mmap syscall
/* mmap that already holds a vfile reference (VMA remap paths); @file is
 * consumed on both success and failure. */
vaddr_t proc_mmap_vfile(vaddr_t addr, size_t len, int prot, int flags,
                        vfile_t *file, long off) {
    task_t *t = proc_current();
    if (!t || !t->mm) {
        vfs_put_file(file);
        return (vaddr_t)-1;
    }

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) {
        vfs_put_file(file);
        return (vaddr_t)-EINVAL;
    }

    uint64_t as_limit = t->limits.as;
    if (as_limit) {
        uint64_t vm_now = t->mm->total_vm;
        uint64_t vm_add = map_len / PAGE_SIZE;
        if (vm_now + vm_add > as_limit / PAGE_SIZE) {
            vfs_put_file(file);
            return (vaddr_t)-ENOMEM;
        }
    }

    mm_tlb_invalidate_begin(t->mm);
    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);
    vaddr_t ret;
    if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1))) {
        spin_unlock_irqrestore(&t->mm->lock, lock_flags);
        mm_tlb_invalidate_finish(t->mm);
        vfs_put_file(file);
        return (vaddr_t)-EINVAL;
    }
    ret = mm_mmap_file_locked(t->mm, addr, len, prot, flags, file,
                              (uint64_t)off);
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    mm_tlb_invalidate_finish(t->mm);
    if ((long)ret < 0 && (long)ret >= -4095)
        ktrace_mm("[MM] mmap-vfile fail pid=%d addr=%lx len=%lu ret=%ld\n",
                  t->pid, (unsigned long)addr, (unsigned long)len, (long)ret);
    return ret;
}

vaddr_t proc_mmap(vaddr_t addr, size_t len, int prot, int flags, int fd, long off) {
    task_t *t = proc_current();
    if (!t || !t->mm) return (vaddr_t)-1;

    size_t map_len = ROUND_UP(len, PAGE_SIZE);
    if (map_len == 0) return (vaddr_t)-EINVAL;

    /* RLIMIT_AS caps the address space, so a single process cannot exhaust
     * memory that other tenants need.  Checked here, before mm->lock, so the
     * rejection path takes no lock; total_vm only grows, so a stale read can
     * only under-count and let a later mapping slip through, never over-count
     * and reject a mapping that would have fit. */
    uint64_t as_limit = t->limits.as;
    if (as_limit) {
        uint64_t vm_now = t->mm->total_vm;
        uint64_t vm_add = map_len / PAGE_SIZE;
        if (vm_now + vm_add > as_limit / PAGE_SIZE)
            return (vaddr_t)-ENOMEM;
    }

    mm_tlb_invalidate_begin(t->mm);
    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);
    vaddr_t ret;
    if ((flags & MAP_ANONYMOUS) || fd < 0)
        ret = mm_mmap_locked(t->mm, addr, len, prot, flags);
    else {
        if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1))) {
            spin_unlock_irqrestore(&t->mm->lock, lock_flags);
            mm_tlb_invalidate_finish(t->mm);
            return (vaddr_t)-EINVAL;
        }

        /* Resolve the caller's fd; mm_mmap_file_locked takes over the
         * reference (success) or drops it (failure). */
        vfile_t *mfile = fdtable_get_current_file_ref(fd);
        if (!mfile) {
            spin_unlock_irqrestore(&t->mm->lock, lock_flags);
            mm_tlb_invalidate_finish(t->mm);
            return (vaddr_t)-EBADF;
        }
        ret = mm_mmap_file_locked(t->mm, addr, len, prot, flags, mfile,
                                  (uint64_t)off);
    }
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    mm_tlb_invalidate_finish(t->mm);
    /* Test the errno range through a signed type of pointer width: an int64_t
     * cast zero-extends on 32-bit and silently makes this dead code. */
    if ((long)ret < 0 && (long)ret >= -4095)
        ktrace_mm("[MM] mmap fail pid=%d comm=%s addr=%lx len=%lu prot=%d "
                  "flags=%x fd=%d ret=%ld\n", t->pid, t->name,
                  (unsigned long)addr, (unsigned long)len, prot,
                  (unsigned)flags, fd, (long)ret);
    return ret;
}

// Remove a memory mapping; the implementation of the munmap syscall
int proc_munmap(vaddr_t addr, size_t len) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -1;
    mm_tlb_invalidate_begin(t->mm);
    uint64_t lock_flags = spin_lock_irqsave(&t->mm->lock);
    int ret = mm_munmap_locked(t->mm, addr, len);
    spin_unlock_irqrestore(&t->mm->lock, lock_flags);
    mm_tlb_invalidate_finish(t->mm);
    return ret;
}

void proc_dump(void) {
    printf("  PID  PPID  STATE  PRI  NAME\n");
    /* E3: snapshot under tasklist_lock, print with no lock held.  proc_dump()
     * writes through the console, so holding the global lock across printf is
     * the self-deadlock this rule exists to prevent. */
    enum { DUMP_SNAP_MAX = 32 };
    static struct {
        int pid;
        int ppid;
        int priority;
        int state;
        char name[16];
    } snap[DUMP_SNAP_MAX];
    int count = 0;

    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        int state = proc_task_state_get(t);
        if (state == PROC_UNUSED) continue;
        if (count >= DUMP_SNAP_MAX) break;
        snap[count].pid = t->pid;
        snap[count].ppid = t->ppid;
        snap[count].priority = t->priority;
        snap[count].state = state;
        strncpy(snap[count].name, t->name, sizeof(snap[count].name) - 1);
        snap[count].name[sizeof(snap[count].name) - 1] = '\0';
        count++;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);

    for (int i = 0; i < count; i++) {
        const char *s = "?";
        switch (snap[i].state) {
            case PROC_READY:   s = "RDY"; break;
            case PROC_RUNNING: s = "RUN"; break;
            case PROC_BLOCKED: s = "BLK"; break;
            case PROC_ZOMBIE:  s = "ZOM"; break;
            default: break;
        }
        printf("  %3d   %3d   %s   %3d  %s\n",
               snap[i].pid, snap[i].ppid, s, snap[i].priority, snap[i].name);
    }
}
