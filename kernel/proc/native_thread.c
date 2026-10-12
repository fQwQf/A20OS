#ifdef CONFIG_NATIVE_FLAT_THREAD
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "ipc/handle_table.h"
#include "core/flat_thread.h"
#include "mm/slab.h"

int proc_create_thread_deferred(uint64_t entry, uint64_t arg, vaddr_t sp,
                                vaddr_t tls, size_t stack_size,
                                task_t **out_task) {
    task_t *parent = proc_current();
    if (!parent || !parent->flat_user || !out_task)
        return -EINVAL;
    int pid = proc_alloc_deferred(arch_flat_program_enter);
    if (pid < 0)
        return pid;
    task_t *child = proc_find_get(pid);
    if (!child)
        return -ESRCH;
    int result =
        arch_flat_thread_attach(parent, child, entry, arg, sp, stack_size, tls);
    if (!result) {
        child->clone_flags =
            CLONE_VM | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
        child->exit_signal = 0;
        child->abi_mode = parent->abi_mode;
        child->tgid = parent->tgid;
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        child->tg_leader = parent->tg_leader ? parent->tg_leader : parent;
        result = proc_tg_link_locked(child);
        spin_unlock_irqrestore(&tasklist_lock, flags);
    }
    if (result) {
        proc_destroy_task(child);
        proc_put(child);
        return result;
    }
    if (parent->signals) {
        if (child->signals &&
            refcount_dec_and_test(
                &((signal_state_t *)child->signals)->refcount))
            kfree(child->signals);
        child->signals = parent->signals;
        refcount_inc(&((signal_state_t *)child->signals)->refcount);
    }
    child->a20_ht = a20_ht_get_ref(task_get_a20_ht(parent));
    proc_set_name(child, parent->name);
    *out_task = child;
    return pid;
}
#endif

#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
/* Preserve the existing in-kernel immediate-publication interface. Native
 * syscalls use the deferred API so handles/copyout can still fail safely. */
int proc_create_thread(uint64_t entry, uint64_t arg, vaddr_t sp, vaddr_t tls) {
    task_t *child = NULL;
    int pid = proc_create_thread_deferred(entry, arg, sp, tls, 0, &child);
    if (pid < 0)
        return pid;
    if (!child)
        return -ENOMEM;
    proc_publish_deferred_task(child);
    proc_put(child);
    return pid;
}
#endif
