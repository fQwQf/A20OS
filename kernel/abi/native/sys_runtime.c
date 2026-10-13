#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "core/runtime.h"
#include "core/errno.h"
#include "proc/proc.h"
#include "proc/task_object.h"
#include "ipc/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "sys/usercopy.h"
int64_t sys_a20_runtime_info(const a20_syscall_args_t *args) {
    void *out = (void *)(uintptr_t)args->arg[0];
    if (!out)
        return -A20_ERR_FAULT;
    if (args->arg[1] < sizeof(a20_runtime_info_t))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_runtime_info_t info;
    a20_runtime_snapshot(&info);
    return copy_to_user(out, &info, sizeof(info)) < 0 ? -A20_ERR_FAULT : A20_OK;
}
int64_t sys_a20_runtime_bind(const a20_syscall_args_t *args) {
    const void *input = (void *)(uintptr_t)args->arg[0];
    if (!input)
        return -A20_ERR_FAULT;
    if (args->arg[1] < sizeof(a20_runtime_binding_t))
        return -A20_ERR_INVALID_ARGUMENT;
    a20_runtime_binding_t binding;
    if (copy_from_user(&binding, input, sizeof(binding)) < 0)
        return -A20_ERR_FAULT;
    if (!a20_runtime_binding_valid(&binding))
        return -A20_ERR_INVALID_ARGUMENT;
    task_t *task = proc_current();
    if (!task || !proc_has_cap(task, CAP_SYS_ADMIN))
        return -A20_ERR_PERM;
    int result = a20_runtime_bind(&binding);
    return result == -EBUSY ? -A20_ERR_BUSY
           : result         ? -A20_ERR_INVALID_ARGUMENT
                            : A20_OK;
}
int64_t sys_a20_handle_identity(const a20_syscall_args_t *args) {
    void *out = (void *)(uintptr_t)args->arg[1];
    if (!out)
        return -A20_ERR_FAULT;
    if (args->arg[2] < sizeof(a20_handle_identity_t))
        return -A20_ERR_INVALID_ARGUMENT;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t entry;
    int64_t result =
        a20_handle_lookup_ref_internal(ht, (a20_handle_t)args->arg[0],
                                       A20_OBJ_INVALID, A20_RIGHT_STAT, &entry);
    if (result < 0)
        return result;
    if (entry.type != A20_OBJ_TASK && entry.type != A20_OBJ_THREAD &&
        entry.type != A20_OBJ_DEBUG) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_NOT_SUPPORTED;
    }
    a20_handle_identity_t info = {.size = A20_HANDLE_IDENTITY_BYTES,
                                  .version = A20_RUNTIME_VERSION};
    if (!a20_runtime_object_namespace(&info.id)) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_WOULD_BLOCK;
    }
    info.id.serial = a20_task_object_serial(entry.object);
    info.id.kind = A20_OBJECT_KIND_TASK;
    task_t *target = a20_task_object_get_task(entry.object);
    info.state = target ? A20_OBJECT_LIVE : A20_OBJECT_RETIRED;
    proc_put(target);
    a20_object_release(entry.object, entry.type);
    return copy_to_user(out, &info, sizeof(info)) < 0 ? -A20_ERR_FAULT : A20_OK;
}
#endif
