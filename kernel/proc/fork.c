#include "proc/resource_ns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/clone_txn.h"
#include "proc/task_construction.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/trap.h"
#include "fs/fdtable.h"
#include "fs/vfs/mntns.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/process_vm.h"
#include "mm/vm.h"
#include "sys/usercopy.h"
#include "ipc/envelope.h"
#include "ipc/keyring.h"
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/handle_table.h"
#endif

int proc_copy_to_task_user(task_t *task, void *dst, const void *src, size_t n)
{
    /* Resolve COW before writing. A direct physical memcpy would also change
     * the parent's private pages and could overwrite the child's results when
     * the syscall subsequently publishes its parent-side output. */
    long copied = process_vm_write_kernel(task, dst, src, n);
    return copied == (long)n ? 0 : -EFAULT;
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

/*
 * kernel/proc/clone_txn.rs owns the fork/clone transaction.  Every adapter
 * below is a mechanical operation this file already performed; the inheritance
 * decisions, the publication boundary, the CLONE_VFORK arming and the rollback
 * order live in Rust.
 */

void a20_clone_view_fill(task_t *t, clone_view_t *out)
{
    out->pid = &t->pid;
    out->tgid = &t->tgid;
    out->ppid = &t->ppid;
    out->parent = (void **)&t->parent;
    out->tg_leader = (void **)&t->tg_leader;
    out->abi_mode = &t->abi_mode;
    out->exit_signal = &t->exit_signal;
    out->clone_flags = &t->clone_flags;
    out->clear_child_tid = &t->clear_child_tid;
    out->sched_policy = &t->sched_policy;
    out->priority = &t->priority;
    out->cfs_weight = &t->cfs_weight;
    out->exec_load_addr = &t->exec_load_addr;
    out->exec_load_size = &t->exec_load_size;
    out->name = t->name;
    out->sched_reset_on_fork = &t->sched_reset_on_fork;
    out->nproc_limit = &t->limits.nproc;
}

int a20_clone_task_pid(const task_t *t)
{
    return t->pid;
}

int a20_clone_task_uid(const task_t *t)
{
    return t->cred.uid;
}

int a20_clone_flags_supported(uint64_t flags)
{
#ifdef CONFIG_NOMMU
    /* A nommu vfork must share the address space; there is no COW copy. */
    if (!(flags & CLONE_VM))
        return -EINVAL;
#else
    (void)flags;
#endif
    return 0;
}

int a20_clone_has_cap(const task_t *t, int cap)
{
    return proc_has_cap(t, cap);
}

uint32_t a20_clone_weight_for_nice(int nice)
{
    return sched_weight_for_nice(nice);
}

void a20_clone_notify_begin(int parent_pid, uint64_t flags)
{
#ifdef CONFIG_COOPERATIVE_BOOT
    kinfo("[PROC] clone begin: parent=%d flags=0x%lx\n",
          parent_pid, (unsigned long)flags);
#else
    (void)parent_pid;
    (void)flags;
#endif
}

void a20_clone_notify_ready(task_t *t, int child_pid, uint64_t flags)
{
#ifdef CONFIG_COOPERATIVE_BOOT
    kinfo("[PROC] clone runnable: child=%d kstack=0x%lx\n",
          child_pid, (unsigned long)t->kstack);
    if (!(flags & CLONE_VFORK))
        proc_yield();
#else
    (void)t;
    (void)child_pid;
    (void)flags;
#endif
}

void a20_clone_inherit_stdio(task_t *t, task_t *parent)
{
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if (parent && parent->a20_stdio_adopted) {
        t->a20_stdio_adopted = 1;
        t->a20_stdin_h = parent->a20_stdin_h;
        t->a20_stdout_h = parent->a20_stdout_h;
        t->a20_stderr_h = parent->a20_stderr_h;
        t->a20_root_h = parent->a20_root_h;
        t->a20_cwd_h = parent->a20_cwd_h;
    }
#else
    (void)t;
    (void)parent;
#endif
}

void a20_clone_inherit_capabilities(task_t *t, task_t *parent, uint64_t flags)
{
    keyring_inherit(t, parent);
    env_inherit_task(t, parent);
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if ((flags & CLONE_THREAD) && parent && parent->abi_mode == 1 &&
        parent->a20_ht) {
        __atomic_store_n(
            &t->a20_ht,
            a20_ht_get_ref((struct a20_ht_internal *)parent->a20_ht),
            __ATOMIC_RELEASE);
    }
#else
    (void)flags;
#endif
}

int a20_clone_init_mm(task_t *t, task_t *parent, uint64_t flags, int child_pid)
{
#ifndef CONFIG_COOPERATIVE_BOOT
    (void)child_pid; /* only the cooperative-boot log prints it */
#endif
    if (!parent->pgdir)
        goto stage_complete;
    if (parent->mm && (flags & CLONE_VM)) {
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
              child_pid, (unsigned long)mm_rss_get(t->mm),
              (unsigned long)t->mm->total_vm);
#endif
    } else {
        t->pgdir = parent->pgdir;
    }
stage_complete:
#ifdef CONFIG_TASK_CONSTRUCTION_TEST
    return proc_task_construction_test_fail_after(parent,
                                                   TASK_CONSTRUCTION_STAGE_MM);
#else
    return 0;
#endif
}

int a20_clone_init_arch(task_t *t, task_t *parent, uint64_t flags, vaddr_t stack,
                        vaddr_t tls)
{
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
        t->ustack = stack ? stack : parent->ustack;
        if (stack)
            TRAP_CTX_SP(trap) = stack;
        /* A forked child keeps the parent's thread-pointer bases (Linux
         * copies fsbase/gsbase into the new task); CLONE_SETTLS replaces the
         * FS base for threads.  The task-struct copies are authoritative:
         * the switch-in TLS restore reads them, and frame slots are rebuilt
         * per trap. */
        t->arch_tls_fs_base = parent->arch_tls_fs_base;
        t->arch_tls_gs_base = parent->arch_tls_gs_base;
        if (flags & CLONE_SETTLS) {
            TRAP_CTX_TP(trap) = tls;
            t->arch_tls_fs_base = tls;
        }
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

uint64_t a20_clone_tasklist_lock(void)
{
    return spin_lock_irqsave(&tasklist_lock);
}

void a20_clone_tasklist_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&tasklist_lock, flags);
}

int a20_clone_vfork_snapshot_create(task_t *parent)
{
#ifdef CONFIG_NOMMU
    return nommu_vfork_snapshot_create(parent);
#else
    (void)parent;
    return 0;
#endif
}

void a20_clone_vfork_arm(task_t *parent)
{
    uint64_t pf = spin_lock_irqsave(&parent->park_lock);
    parent->vfork_waiting = 1;
    spin_unlock_irqrestore(&parent->park_lock, pf);
}

void a20_clone_vfork_disarm(task_t *parent)
{
    uint64_t pf = spin_lock_irqsave(&parent->park_lock);
    parent->vfork_waiting = 0;
    spin_unlock_irqrestore(&parent->park_lock, pf);
#ifdef CONFIG_NOMMU
    nommu_vfork_snapshot_discard(parent);
#endif
}

void a20_clone_vfork_wait(task_t *child)
{
    wait_for_completion(&child->vfork_done);
    proc_put(child);
}

int proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid,
               int exit_signal)
{
    return a20_proc_clone_impl(flags, stack, ptid, tls, ctid, exit_signal, NULL);
}

/*
 * Deferred-ready clone for the Native ABI task_clone: creates the child
 * (COW address space, register continuation, fd/signal state) but does NOT
 * make it runnable.  The caller installs the child's handle table and then
 * calls proc_make_ready().  Returns the child pid; *out_task is referenced.
 */
int proc_clone_deferred(vaddr_t stack, task_t **out_task)
{
    return a20_proc_clone_impl(0, stack, NULL, 0, NULL, SIGCHLD, out_task);
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
    int pid = a20_proc_clone_impl(flags, sp, NULL, tls, NULL, 0, &t);
    if (pid < 0 || !t)
        return pid < 0 ? pid : -ENOMEM;

    trap_context_t *tc = t->trap_ctx;
    TRAP_CTX_EPC(tc) = entry;
    TRAP_CTX_SET_ARG0(tc, arg);
    *out_task = proc_get(t);
    return pid;
}
