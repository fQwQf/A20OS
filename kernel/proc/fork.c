#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/trap.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "sys/usercopy.h"

#define CLONE_VM             0x00000100
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

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

int proc_clone(uint64_t flags, uint64_t stack, int *ptid, uint64_t tls, int *ctid)
{
    task_t *parent = proc_current();
    task_t *t = proc_alloc_task_slot();
    if (!t)
        return -EAGAIN;

    t->pid = proc_pid_alloc();
    if (t->pid < 0) {
        proc_destroy_task(t);
        return -EAGAIN;
    }
    proc_task_init_common(t, parent);
    proc_pid_register(t);

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
        TRAP_CTX_ARG0(trap) = 0;
        TRAP_CTX_KScratch0(trap) = t->pgdir ? arch_make_satp(t->pgdir) : 0;
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, ks_top);
        t->trap_ctx = trap;
        t->ustack = stack ? stack : parent->ustack;
        if (stack)
            TRAP_CTX_SP(trap) = stack;
        if (flags & CLONE_SETTLS)
            TRAP_CTX_TP(trap) = tls;

        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)t;
        TASK_CTX_PAGE_TABLE(ctx) = t->pgdir ? arch_make_satp(t->pgdir) : 0;
        TASK_CTX_STATUS(ctx) = TRAP_CTX_STATUS(trap);
        t->kstack = (uint64_t)ctx;
    } else {
        task_context_t *ctx = (task_context_t *)(ks_top - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra = (uint64_t)idle_loop;
        ctx->tp = (uint64_t)t;
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
        mm_struct_t *shared_mm = (flags & CLONE_VM) ? parent->mm : NULL;
        while (t->state != PROC_UNUSED && t->state != PROC_ZOMBIE &&
               (!shared_mm || t->mm == shared_mm))
            proc_yield();
    }
    return t->pid;
}
