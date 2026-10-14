#include "proc/resource_ns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/task_construction.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/trap.h"
#include "fs/fdtable.h"
#include "fs/vfs/mntns.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "sys/usercopy.h"
#include "ipc/envelope.h"
#include "ipc/keyring.h"
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/handle_table.h"
#endif

int proc_copy_to_task_user(task_t *task, void *dst, const void *src, size_t n)
{
    if (!task || !task->pgdir)
        return -EFAULT;

    size_t done = 0;
    while (done < n) {
        vaddr_t va = (vaddr_t)(uintptr_t)dst + done;
        paddr_t pa = pt_translate(task->pgdir, va);
        if (!pa)
            return -EFAULT;
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn))
            return -EFAULT;
        char *kv = (char *)pfn_to_virt(pfn) + (pa & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n - done)
            chunk = n - done;
        memcpy(kv, (const char *)src + done, chunk);
        done += chunk;
    }
    return 0;
}

#ifdef CONFIG_NOMMU
static void nommu_vfork_snapshot_discard(task_t *parent)
{
    if (!parent || !parent->nommu_vfork_snaps)
        return;

    for (int i = 0; i < parent->nommu_num_vfork_snapshots; i++)
        kfree(parent->nommu_vfork_snaps[i].data);
    kfree(parent->nommu_vfork_snaps);
    parent->nommu_vfork_snaps = NULL;
    parent->nommu_num_vfork_snapshots = 0;
}

static int nommu_vfork_snapshot_create(task_t *parent)
{
    if (!parent || !parent->mm)
        return 0;
    if (parent->nommu_vfork_snaps)
        return -EBUSY;

    int count = 0;
    for (int i = 0; i < parent->mm->num_nommu_allocs; i++) {
        if (parent->mm->nommu_allocs[i] &&
            parent->mm->nommu_alloc_sizes[i])
            count++;
    }
    for (mm_seg_t *v = parent->mm->mmap; v; v = v->next) {
        if (v->nommu_alloc && (v->vm_flags & VM_WRITE))
            count++;
    }
    if (count == 0)
        return 0;

    nommu_vfork_snap_entry_t *snaps =
        kcalloc((size_t)count, sizeof(*snaps));
    if (!snaps)
        return -ENOMEM;

    parent->nommu_vfork_snaps = snaps;
    for (int i = 0; i < parent->mm->num_nommu_allocs; i++) {
        void *src = parent->mm->nommu_allocs[i];
        size_t size = parent->mm->nommu_alloc_sizes[i];
        if (!src || !size)
            continue;

        void *copy = kmalloc(size);
        if (!copy)
            goto fail;
        memcpy(copy, src, size);
        snaps[parent->nommu_num_vfork_snapshots++] =
            (nommu_vfork_snap_entry_t){ .dst = src, .data = copy, .size = size };
    }
    for (mm_seg_t *v = parent->mm->mmap; v; v = v->next) {
        if (!v->nommu_alloc || !(v->vm_flags & VM_WRITE))
            continue;

        size_t size = (size_t)(v->end - v->start);
        void *src = (void *)(uintptr_t)v->start;
        void *copy = kmalloc(size);
        if (!copy)
            goto fail;
        memcpy(copy, src, size);
        snaps[parent->nommu_num_vfork_snapshots++] =
            (nommu_vfork_snap_entry_t){ .dst = src, .data = copy, .size = size };
    }
    return 0;

fail:
    nommu_vfork_snapshot_discard(parent);
    return -ENOMEM;
}
#endif

/* Count live tasks owned by @uid, for RLIMIT_NPROC.  Linux scopes that limit
 * to the caller's real uid rather than per-process, so a fork bomb from one
 * account is capped no matter how many processes it already has. */
static uint32_t proc_count_tasks_for_uid(int uid)
{
    uint32_t n = 0;
    /* E2: tasklist_lock walks the list; each task's ->state is sampled under
     * its own park_lock, one at a time.  ->cred.uid is fixed at clone time. */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        int st = proc_task_state_get(t);
        if (st == PROC_UNUSED || st == PROC_ZOMBIE)
            continue;
        if (t->cred.uid == uid)
            n++;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return n;
}

typedef struct proc_clone_transaction {
    uint64_t flags;
    vaddr_t stack;
    int *ptid;
    vaddr_t tls;
    int *ctid;
    int exit_signal;
    task_t **out_task;
    task_t *parent;
    task_t *task;
    task_t *vfork_child_ref;
    int child_pid;
    int parent_vfork_armed;
} proc_clone_transaction_t;

static int clone_validate(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    tx->parent = proc_current();
    if (!tx->parent)
        return -ESRCH;
#ifdef CONFIG_COOPERATIVE_BOOT
    kinfo("[PROC] clone begin: parent=%d flags=0x%lx\n",
          tx->parent->pid, (unsigned long)tx->flags);
#endif
#ifdef CONFIG_NOMMU
    if (!(tx->flags & CLONE_VM))
        return -EINVAL;
#endif
    if (tx->parent->limits.nproc &&
        !proc_has_cap(tx->parent, CAP_SYS_RESOURCE) &&
        proc_count_tasks_for_uid(tx->parent->cred.uid) >=
            tx->parent->limits.nproc)
        return -EAGAIN;
    if (tx->exit_signal < 0 || tx->exit_signal >= NSIG)
        return -EINVAL;
    return 0;
}

static int clone_allocate(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    tx->task = proc_alloc_task_slot();
    if (!tx->task)
        return -EAGAIN;
    tx->task->pid = proc_pid_alloc();
    if (tx->task->pid < 0)
        return -EAGAIN;
    tx->child_pid = tx->task->pid;
    return 0;
}

static int clone_base(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    task_t *parent = tx->parent;
    proc_task_init_base(t, parent);
    t->abi_mode = parent ? parent->abi_mode : 0;
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if (parent && parent->a20_stdio_adopted) {
        t->a20_stdio_adopted = 1;
        t->a20_stdin_h = parent->a20_stdin_h;
        t->a20_stdout_h = parent->a20_stdout_h;
        t->a20_stderr_h = parent->a20_stderr_h;
        t->a20_root_h = parent->a20_root_h;
        t->a20_cwd_h = parent->a20_cwd_h;
    }
#endif
    if (parent && parent->sched_reset_on_fork) {
        if (parent->sched_policy == SCHED_FIFO || parent->sched_policy == SCHED_RR) {
            t->sched_policy = SCHED_NORMAL;
            t->priority = 0;
            t->cfs_weight = sched_weight_for_nice(0);
        } else if (t->priority < 0) {
            t->priority = 0;
            t->cfs_weight = sched_weight_for_nice(0);
        }
    }
    t->exit_signal = (tx->flags & CLONE_THREAD) ? 0 : tx->exit_signal;
    if ((tx->flags & CLONE_PARENT) && parent && parent->parent) {
        t->parent = parent->parent;
        t->ppid = parent->parent->pid;
    }
    if (tx->flags & CLONE_THREAD)
        t->tgid = parent ? parent->tgid : t->pid;
    t->clone_flags = (int)tx->flags;
    if (tx->flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = tx->ctid;
    proc_set_name(t, parent->name);
    t->exec_load_addr = parent->exec_load_addr;
    t->exec_load_size = parent->exec_load_size;
    return 0;
}

static int clone_seccomp(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    proc_task_init_seccomp(tx->task, tx->parent);
    return 0;
}

static int clone_fs_cgroup(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    proc_task_init_fs_cgroup(tx->task, tx->parent);
    return 0;
}

static int clone_files(void *opaque)
{
#ifndef CONFIG_MCU
    proc_clone_transaction_t *tx = opaque;
    if (!tx->parent)
        return 0;
    if (tx->flags & (CLONE_FILES | CLONE_THREAD)) {
        fdtable_share(tx->task, tx->parent);
        return 0;
    }
    return fdtable_copy_checked(tx->task, tx->parent);
#else
    (void)opaque;
    return 0;
#endif
}

static int clone_signals(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    return proc_task_init_signals(tx->task, tx->parent, 1);
}

static int clone_namespaces(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    task_t *parent = tx->parent;
    int rc = mntns_fork(t, parent, tx->flags);
    if (rc < 0)
        return -ENOMEM;
    rc = pidns_fork(t, parent, tx->flags);
    if (rc < 0)
        return rc;
    rc = userns_fork(t, parent, tx->flags);
    if (rc < 0)
        return rc;
    return resource_ns_fork(t, parent, tx->flags);
}

static int clone_capabilities(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    task_t *parent = tx->parent;
    keyring_inherit(t, parent);
    env_inherit_task(t, parent);
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if ((tx->flags & CLONE_THREAD) && parent && parent->abi_mode == 1 &&
        parent->a20_ht) {
        __atomic_store_n(
            &t->a20_ht,
            a20_ht_get_ref((struct a20_ht_internal *)parent->a20_ht),
            __ATOMIC_RELEASE);
    }
#endif
    return 0;
}

static int clone_mm(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    task_t *parent = tx->parent;
    if (!parent->pgdir)
        goto stage_complete;
    if (parent->mm && (tx->flags & CLONE_VM)) {
        t->mm = parent->mm;
        refcount_inc(&t->mm->refcount);
        t->pgdir = parent->pgdir;
    } else if (parent->mm) {
        t->mm = mm_fork(parent->mm);
        if (!t->mm)
            return -ENOMEM;
        t->pgdir = t->mm->pgdir;
#ifdef CONFIG_COOPERATIVE_BOOT
        kinfo("[PROC] clone mm ready: child=%d rss=%lu vm=%lu\n",
              tx->child_pid, (unsigned long)mm_rss_get(t->mm),
              (unsigned long)t->mm->total_vm);
#endif
    } else {
        t->pgdir = parent->pgdir;
    }
stage_complete:
#ifdef CONFIG_TASK_CONSTRUCTION_TEST
    return proc_task_construction_test_fail_after(tx->parent,
                                                   TASK_CONSTRUCTION_STAGE_MM);
#else
    return 0;
#endif
}

static int clone_arch(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    task_t *parent = tx->parent;
    void *kstack = kmalloc(KERNEL_STACK_SIZE);
    if (!kstack)
        return -ENOMEM;
    memset(kstack, 0, KERNEL_STACK_SIZE);
    t->kstack_base = kstack;
    uint64_t ks_top = (uint64_t)(uintptr_t)kstack + KERNEL_STACK_SIZE;
    if (parent->trap_ctx) {
        trap_context_t *trap = (trap_context_t *)(uintptr_t)(ks_top - sizeof(trap_context_t));
        *trap = *parent->trap_ctx;
        TRAP_CTX_SET_RET(trap, 0);
        uint64_t child_as = t->mm ? mm_address_space_token(t->mm) :
            (t->pgdir ? arch_make_addr_space_token(t->pgdir) : 0);
        TRAP_CTX_KScratch0(trap) = child_as;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, ks_top);
        t->trap_ctx = trap;
        t->ustack = tx->stack ? tx->stack : parent->ustack;
        if (tx->stack)
            TRAP_CTX_SP(trap) = tx->stack;
        if (tx->flags & CLONE_SETTLS)
            TRAP_CTX_TP(trap) = tx->tls;
        task_context_t *ctx = arch_task_context_base(kstack, ks_top, trap);
        t->first_kernel_entry = (uintptr_t)user_trap_return;
        ctx->ra = (uint64_t)(uintptr_t)proc_task_first_entry;
        ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_user_tp(ctx, TRAP_CTX_TP(trap));
        TASK_CTX_PAGE_TABLE(ctx) = child_as;
        TASK_CTX_STATUS(ctx) = arch_task_user_resume_status();
        arch_task_context_set_initial_sp(ctx, trap, ks_top);
        arch_task_context_copy_user_fp(ctx);
        t->kstack = (uint64_t)(uintptr_t)ctx;
    } else {
        task_context_t *ctx = arch_task_context_base(kstack, ks_top, NULL);
        memset(ctx, 0, sizeof(*ctx));
        t->first_kernel_entry = (uintptr_t)idle_loop;
        ctx->ra = (uint64_t)(uintptr_t)proc_task_first_entry;
        ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_initial_sp(ctx, NULL, ks_top);
        t->kstack = (uint64_t)(uintptr_t)ctx;
    }
    return 0;
}

static int clone_publish(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    uint64_t list_flags = spin_lock_irqsave(&tasklist_lock);
    int rc = proc_publish_task_slot_locked(t);
    if (!rc && (tx->flags & CLONE_THREAD)) {
        if (tx->parent && tx->parent->tg_leader)
            t->tg_leader = tx->parent->tg_leader;
        else if (tx->parent)
            t->tg_leader = tx->parent;
        else
            t->tg_leader = t;
    } else if (!rc) {
        t->tg_leader = t;
    }
    if (!rc && t->tg_leader != t)
        rc = proc_tg_link_locked(t);
    if (!rc)
        proc_children_link_locked(t->parent, t);
    spin_unlock_irqrestore(&tasklist_lock, list_flags);
    if (rc < 0)
        return rc;
    proc_pid_register(t);
    return 0;
}

static int clone_post_publish(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    task_t *t = tx->task;
    if ((tx->flags & CLONE_PARENT_SETTID) && tx->ptid) {
        int tid = t->pid;
        if (copy_to_user(tx->ptid, &tid, sizeof(tid)) < 0)
            return -EFAULT;
    }
    if ((tx->flags & CLONE_CHILD_SETTID) && tx->ctid) {
        int tid = t->pid;
        if (proc_copy_to_task_user(t, tx->ctid, &tid, sizeof(tid)) < 0)
            return -EFAULT;
    }
#ifdef CONFIG_NOMMU
    if (tx->flags & CLONE_VFORK) {
        int rc = nommu_vfork_snapshot_create(tx->parent);
        if (rc < 0)
            return rc;
    }
#endif
    if (tx->flags & CLONE_VFORK) {
        uint64_t pf = spin_lock_irqsave(&tx->parent->park_lock);
        tx->parent->vfork_waiting = 1;
        spin_unlock_irqrestore(&tx->parent->park_lock, pf);
        tx->parent_vfork_armed = 1;
        tx->vfork_child_ref = proc_get(t);
        if (!tx->vfork_child_ref)
            return -ESRCH;
    }
    if (tx->out_task)
        *tx->out_task = t;
    return 0;
}

static int clone_ready(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    proc_make_ready(tx->task);
#ifdef CONFIG_COOPERATIVE_BOOT
    kinfo("[PROC] clone runnable: child=%d kstack=0x%lx\n",
          tx->child_pid, (unsigned long)tx->task->kstack);
    if (!(tx->flags & CLONE_VFORK))
        proc_yield();
#endif
    return 0;
}

static void clone_vfork_wait(void *opaque)
{
    proc_clone_transaction_t *tx = opaque;
    if (!(tx->flags & CLONE_VFORK))
        return;
    wait_for_completion(&tx->vfork_child_ref->vfork_done);
    proc_put(tx->vfork_child_ref);
    tx->vfork_child_ref = NULL;
}

static void clone_abort(void *opaque, uint8_t publication_attempted)
{
    proc_clone_transaction_t *tx = opaque;
    (void)publication_attempted;
    if (!tx->task)
        return;
    if (tx->parent_vfork_armed) {
        uint64_t pf = spin_lock_irqsave(&tx->parent->park_lock);
        tx->parent->vfork_waiting = 0;
        spin_unlock_irqrestore(&tx->parent->park_lock, pf);
#ifdef CONFIG_NOMMU
        nommu_vfork_snapshot_discard(tx->parent);
#endif
        tx->parent_vfork_armed = 0;
    }
    if (tx->vfork_child_ref) {
        proc_put(tx->vfork_child_ref);
        tx->vfork_child_ref = NULL;
    }
    proc_destroy_task(tx->task);
    tx->task = NULL;
}

static int proc_clone_impl(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls,
                           int *ctid, int exit_signal, task_t **out_task)
{
    proc_clone_transaction_t tx = {
        .flags = flags,
        .stack = stack,
        .ptid = ptid,
        .tls = tls,
        .ctid = ctid,
        .exit_signal = exit_signal,
        .out_task = out_task,
    };
    task_construction_ops_t ops = {
        .context = &tx,
        .validate = clone_validate,
        .allocate = clone_allocate,
        .base = clone_base,
        .seccomp = clone_seccomp,
        .fs_cgroup = clone_fs_cgroup,
        .files = clone_files,
        .signals = clone_signals,
        .namespaces = clone_namespaces,
        .capabilities = clone_capabilities,
        .mm = clone_mm,
        .arch = clone_arch,
        .publish = clone_publish,
        .post_publish = clone_post_publish,
        .ready = clone_ready,
        .vfork_wait = clone_vfork_wait,
        .abort = clone_abort,
        .deferred = out_task != NULL,
        .vfork = (flags & CLONE_VFORK) != 0,
    };
    int rc = proc_task_construction_run(&ops);
    return rc < 0 ? rc : tx.child_pid;
}

int proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid,
                 int exit_signal)
{
    return proc_clone_impl(flags, stack, ptid, tls, ctid, exit_signal, NULL);
}

/*
 * Deferred-ready clone for the Native ABI task_clone: creates the child
 * (COW address space, register continuation, fd/signal state) but does NOT
 * make it runnable.  The caller installs the child's handle table and then
 * calls proc_make_ready().  Returns the child pid; *out_task is referenced.
 */
int proc_clone_deferred(vaddr_t stack, task_t **out_task)
{
    return proc_clone_impl(0, stack, NULL, 0, NULL, SIGCHLD, out_task);
}

/*
 * Native ABI thread creation: the new thread shares the caller's address
 * space, fd table, signal handlers and (for native tasks) handle table, and
 * starts executing at `entry` with `arg` in the first argument register.
 */
int proc_create_thread_deferred(uint64_t entry, uint64_t arg, vaddr_t sp,
                                vaddr_t tls, size_t stack_size,
                                task_t **out_task) {
    (void)stack_size;
    if (!out_task || sp <= ARCH_NATIVE_THREAD_STACK_BIAS || (sp & 15U))
        return -EINVAL;
    sp = arch_native_thread_initial_sp(sp);
    task_t *t = NULL;
    uint64_t flags = CLONE_VM | CLONE_FILES | CLONE_SIGHAND |
                     CLONE_THREAD | CLONE_SETTLS;
    int pid = proc_clone_impl(flags, sp, NULL, tls, NULL, 0, &t);
    if (pid < 0 || !t)
        return pid < 0 ? pid : -ENOMEM;

    trap_context_t *tc = t->trap_ctx;
    TRAP_CTX_EPC(tc) = entry;
    TRAP_CTX_SET_ARG0(tc, arg);
    *out_task = proc_get(t);
    return pid;
}
