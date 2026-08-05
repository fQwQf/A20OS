/*
 * A20OS Native ABI — user-space driver (udriver) syscalls.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §3.2.
 * The kernel authorizes device windows and delivers IRQs to event
 * queues; device logic lives in user tasks.
 */
#include "core/types.h"
#include "core/string.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/ipc_internal.h"
#include "drivers/core/udriver.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern void a20_object_release(void *object, uint16_t type);

int64_t sys_a20_device_map_mmio(const a20_syscall_args_t *args)
{
    a20_device_map_mmio_args_t *uargs =
        (a20_device_map_mmio_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_map_mmio_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    uint64_t va = 0;
    if (udriver_map_mmio(cur->mm, kargs.phys_base, kargs.length,
                         kargs.prot, &va) < 0)
        return -A20_ERR_ACCESS;

    kargs.out_addr = va;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_device_irq_listen(const a20_syscall_args_t *args)
{
    a20_device_irq_listen_args_t *uargs =
        (a20_device_irq_listen_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_irq_listen_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.queue,
                                               A20_OBJ_EVENT_QUEUE,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    if (udriver_irq_listen(kargs.irq, eq, kargs.user_data, cur->pid) < 0) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_ACCESS;
    }

    /* Bind the IRQ key to the queue so SIGNALED events are delivered. */
    int64_t wr = a20_eventq_watch(eq, (a20_handle_t)kargs.irq,
                                  (void *)(uintptr_t)kargs.irq,
                                  A20_OBJ_DEVICE,
                                  A20_EVENT_MASK(A20_EVENT_SIGNALED),
                                  kargs.user_data);
    a20_object_release(entry.object, entry.type);
    if (wr < 0) {
        udriver_irq_unlisten(kargs.irq);
        return wr;
    }
    return A20_OK;
}

int64_t sys_a20_device_irq_ack(const a20_syscall_args_t *args)
{
    uint32_t irq = (uint32_t)A20_ARG(0);
    if (udriver_irq_ack(irq) < 0)
        return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_device_irq_unlisten(const a20_syscall_args_t *args)
{
    uint32_t irq = (uint32_t)A20_ARG(0);
    if (udriver_irq_unlisten(irq) < 0)
        return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}
