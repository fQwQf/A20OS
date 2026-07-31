#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "core/consts.h"
#include "core/string.h"
#include "core/trap.h"
#include "fs/fdtable.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "sys/usercopy.h"
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "abi/native/handle_table.h"
#endif

static int proc_copy_to_task_user(task_t *task, void *dst, const void *src, size_t n)
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
    for (vm_area_t *v = parent->mm->mmap; v; v = v->next) {
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
    for (vm_area_t *v = parent->mm->mmap; v; v = v->next) {
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

static int proc_clone_impl(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid,
                 int exit_signal, task_t **out_task)
{
    task_t *parent = proc_current();
#ifdef CONFIG_AARCH64_COOPERATIVE_BOOT
    kinfo("[PROC] clone begin: parent=%d flags=0x%lx\n",
          parent ? parent->pid : -1, (unsigned long)flags);
#endif

#ifdef CONFIG_NOMMU
    /*
     * A NOMMU child cannot own a private copy of an address space.  Enforce
     * that invariant in the process core so every ABI gets the same behavior
     * and mm_fork() can never duplicate allocation ownership.
     */
    if (!(flags & CLONE_VM))
        return -EINVAL;
#endif

    task_t *t = proc_alloc_task_slot();
    if (!t)
        return -EAGAIN;

    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    int child_pid = t->pid;
    proc_task_init_common(t, parent);
    t->abi_mode = parent ? parent->abi_mode : 0;
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
    proc_pid_register(t);

    if (exit_signal < 0 || exit_signal >= NSIG) {
        proc_destroy_task(t);
        return -EINVAL;
    }
    t->exit_signal = (flags & CLONE_THREAD) ? 0 : exit_signal;

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

    t->exec_load_addr = parent->exec_load_addr;
    t->exec_load_size = parent->exec_load_size;

    /*
     * Share fd table when CLONE_FILES or CLONE_THREAD is set.
     * Linux pthreads always pass CLONE_FILES, but some callers
     * only set CLONE_THREAD.  Both must share the same fd table.
     */
    if ((flags & (CLONE_FILES | CLONE_THREAD)) && parent && parent->files) {
        fdtable_share(t, parent);
    }

    /*
     * Share signal handlers when CLONE_SIGHAND or CLONE_THREAD is set.
     * Per-thread pending signals remain separate (each task has its own
     * pending mask inside the shared signal_state), but sigaction
     * entries must be shared across all threads.
     */
    if ((flags & (CLONE_SIGHAND | CLONE_THREAD)) && parent && parent->signals) {
        if (t->signals)
            kfree(t->signals);
        t->signals = parent->signals;
        refcount_inc(&((signal_state_t *)t->signals)->refcount);
    }

    /*
     * Native ABI: the handle table is process-local (docs/native-abi/03-handle.md
     * §2), so threads share it and hold one reference each.
     */
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    if ((flags & CLONE_THREAD) && parent && parent->abi_mode == 1 &&
        parent->scratch_buf) {
        __atomic_store_n(
            &t->scratch_buf,
            a20_ht_get_ref((struct a20_ht_internal *)parent->scratch_buf),
            __ATOMIC_RELEASE);
    }
#endif

    if (parent->pgdir) {
        if (parent->mm && (flags & CLONE_VM)) {
            t->mm = parent->mm;
            refcount_inc(&t->mm->refcount);
            t->pgdir = parent->pgdir;
        } else if (parent->mm) {
            t->mm = mm_fork(parent->mm);
            if (!t->mm) {
                proc_destroy_task(t);
                return -ENOMEM;
            }
            t->pgdir = t->mm->pgdir;
#ifdef CONFIG_AARCH64_COOPERATIVE_BOOT
            kinfo("[PROC] clone mm ready: child=%d rss=%lu vm=%lu\n",
                  child_pid, (unsigned long)t->mm->rss,
                  (unsigned long)t->mm->total_vm);
#endif
        } else {
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
        TRAP_CTX_SET_RET(trap, 0);
        TRAP_CTX_KScratch0(trap) = t->pgdir ? arch_make_addr_space_token(t->pgdir) : 0;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, ks_top);
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;
        if (stack)
            TRAP_CTX_SP(trap) = stack;
        if (flags & CLONE_SETTLS)
            TRAP_CTX_TP(trap) = tls;

        /*
         * Ask the architecture where the task_context_t belongs.  x86_64
         * places it at the bottom of the kernel stack; the other arches place
         * it just below the pre-allocated trap frame.
         */
        task_context_t *ctx = arch_task_context_base(kstack, ks_top, trap);
        t->first_kernel_entry = (uintptr_t)user_trap_return;
        ctx->ra = (uint64_t)proc_task_first_entry;
        ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_user_tp(ctx, TRAP_CTX_TP(trap));
        TASK_CTX_PAGE_TABLE(ctx) = t->pgdir ? arch_make_addr_space_token(t->pgdir) : 0;
        TASK_CTX_STATUS(ctx) = arch_task_user_resume_status();
        arch_task_context_set_initial_sp(ctx, trap, ks_top);
        t->kstack = (uint64_t)ctx;
    } else {
        task_context_t *ctx = arch_task_context_base(kstack, ks_top, NULL);
        memset(ctx, 0, sizeof(*ctx));
        t->first_kernel_entry = (uintptr_t)idle_loop;
        ctx->ra = (uint64_t)proc_task_first_entry;
        ctx->tp = (uint64_t)t;
        arch_task_context_set_initial_sp(ctx, NULL, ks_top);
        t->kstack = (uint64_t)ctx;
    }

    if ((flags & CLONE_PARENT_SETTID) && ptid) {
        int child_tid = t->pid;
        if (copy_to_user(ptid, &child_tid, sizeof(child_tid)) < 0) {
            proc_destroy_task(t);
            return -EFAULT;
        }
    }
    if ((flags & CLONE_CHILD_SETTID) && ctid) {
        int child_tid = t->pid;
        if (proc_copy_to_task_user(t, ctid, &child_tid, sizeof(child_tid)) < 0) {
            proc_destroy_task(t);
            return -EFAULT;
        }
    }

#ifdef CONFIG_NOMMU
    if (flags & CLONE_VFORK) {
        int snapshot_ret = nommu_vfork_snapshot_create(parent);
        if (snapshot_ret < 0) {
            proc_destroy_task(t);
            return snapshot_ret;
        }
    }
#endif

    if (flags & CLONE_VFORK) {
        uint64_t pf = spin_lock_irqsave(&proc_lock);
        parent->vfork_waiting = 1;
        spin_unlock_irqrestore(&proc_lock, pf);
    }

    /*
     * The completion is embedded in the child task.  Once the child is made
     * runnable it may exit, complete vfork_done, and be auto-reaped by another
     * CPU before the parent has unlinked its completion wait entry.  Pin the
     * child across the whole wait so the completion object remains alive.
     */
    task_t *vfork_child_ref = NULL;
    if (flags & CLONE_VFORK) {
        vfork_child_ref = proc_get(t);
        if (!vfork_child_ref) {
            uint64_t pf = spin_lock_irqsave(&proc_lock);
            parent->vfork_waiting = 0;
            spin_unlock_irqrestore(&proc_lock, pf);
#ifdef CONFIG_NOMMU
            nommu_vfork_snapshot_discard(parent);
#endif
            proc_destroy_task(t);
            return -ESRCH;
        }
    }

    /*
     * Deferred-ready path (native thread creation): the caller finalizes the
     * child's trap frame (entry PC, argument register) and then calls
     * proc_make_ready() itself.  Never combined with CLONE_VFORK.
     */
    if (out_task) {
        *out_task = t;
        return child_pid;
    }

    proc_make_ready(t);
#ifdef CONFIG_AARCH64_COOPERATIVE_BOOT
    kinfo("[PROC] clone runnable: child=%d kstack=0x%lx\n",
          child_pid, (unsigned long)t->kstack);

    /* VirtualBox ARM does not expose a usable architectural timer, so there
     * is no interrupt which can preempt the parent after clone returns.  Run
     * the newly published child once while both trap frames are still valid.
     * The parent resumes here when the child next yields or blocks, exactly as
     * it would after a timer-driven context switch on the normal boards. */
    if (!(flags & CLONE_VFORK))
        proc_yield();
#endif
    if (flags & CLONE_VFORK) {
        wait_for_completion(&vfork_child_ref->vfork_done);
        proc_put(vfork_child_ref);
    }
    return child_pid;
}

int proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid,
                 int exit_signal)
{
    return proc_clone_impl(flags, stack, ptid, tls, ctid, exit_signal, NULL);
}

/*
 * Native ABI thread creation: the new thread shares the caller's address
 * space, fd table, signal handlers and (for native tasks) handle table, and
 * starts executing at `entry` with `arg` in the first argument register.
 */
int proc_create_thread(uint64_t entry, uint64_t arg, vaddr_t sp, vaddr_t tls)
{
    task_t *t = NULL;
    uint64_t flags = CLONE_VM | CLONE_FILES | CLONE_SIGHAND |
                     CLONE_THREAD | CLONE_SETTLS;
    int pid = proc_clone_impl(flags, sp, NULL, tls, NULL, 0, &t);
    if (pid < 0 || !t)
        return pid < 0 ? pid : -ENOMEM;

    trap_context_t *tc = t->trap_ctx;
    TRAP_CTX_EPC(tc) = entry;
    TRAP_CTX_SET_ARG0(tc, arg);
    proc_make_ready(t);
    return pid;
}
