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

int proc_clone(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls, int *ctid,
                int exit_signal)
{
    task_t *parent = proc_current();

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
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_user_tp(ctx, TRAP_CTX_TP(trap));
        TASK_CTX_PAGE_TABLE(ctx) = t->pgdir ? arch_make_addr_space_token(t->pgdir) : 0;
        TASK_CTX_STATUS(ctx) = TRAP_CTX_STATUS(trap);
        arch_task_context_set_initial_sp(ctx, trap, ks_top);
        t->kstack = (uint64_t)ctx;
    } else {
        task_context_t *ctx = arch_task_context_base(kstack, ks_top, NULL);
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra = (uint64_t)idle_loop;
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
        parent->state = PROC_BLOCKED;
        spin_unlock_irqrestore(&proc_lock, pf);
    }

    proc_make_ready(t);
    if (flags & CLONE_VFORK) {
        for (;;) {
            uint64_t pf = spin_lock_irqsave(&proc_lock);
            int done = (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
                        !(t->clone_flags & CLONE_VFORK));
            if (done) {
                parent->vfork_waiting = 0;
                spin_unlock_irqrestore(&proc_lock, pf);
                break;
            }
            spin_unlock_irqrestore(&proc_lock, pf);
            sched();
        }
    }
    return child_pid;
}
