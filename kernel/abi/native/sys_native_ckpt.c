/*
 * Native ABI thin wrappers for service checkpoint/restore.  All logic lives
 * in kernel/proc/checkpoint.c and kernel/proc/ckpt_restore.c; here we only
 * copy the path in from user memory and map errnos.
 */

#include "abi/native/syscall_entry.h"
#include "core/errno.h"
#include "ipc/ipc.h"
#include "proc/checkpoint.h"
#include "fs/vfs.h"

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);

#define A20_ARG(n) (args->arg[(n)])

static int64_t ckpt_native_status(int rc)
{
    if (rc >= 0)
        return rc;
    switch (rc) {
    case -EPERM:  return -A20_ERR_PERM;
    case -ESRCH:  return -A20_ERR_NO_ENTRY;
    case -EBADF:
    case -EINVAL:
    case -ENOEXEC: return -A20_ERR_INVALID_ARGUMENT;
    case -EFAULT: return -A20_ERR_FAULT;
    case -ENOMEM: return -A20_ERR_NO_MEMORY;
    case -EBUSY:  return -A20_ERR_BUSY;
    case -E2BIG:  return -A20_ERR_NO_SPACE;
    case -ETIMEDOUT: return -A20_ERR_TIMED_OUT;
    case -EOPNOTSUPP:
    case -ENOSYS: return -A20_ERR_NOT_SUPPORTED;
    default:      return -A20_ERR_IO;
    }
}

int64_t sys_a20_task_checkpoint(const a20_syscall_args_t *args)
{
    int pid = (int)A20_ARG(0);
    const char *upath = (const char *)(uintptr_t)A20_ARG(1);
    uint32_t plen = (uint32_t)A20_ARG(2);
    uint32_t flags = (uint32_t)A20_ARG(3);
    char path[MAX_PATH_LEN];
    if (!upath || !plen)
        return -A20_ERR_INVALID_ARGUMENT;
    if (copy_path_from_user(path, upath, plen) < 0)
        return -A20_ERR_FAULT;
    return ckpt_native_status(proc_ckpt_checkpoint_path(pid, path, flags));
}

int64_t sys_a20_task_restore(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)(uintptr_t)A20_ARG(0);
    uint32_t plen = (uint32_t)A20_ARG(1);
    char path[MAX_PATH_LEN];
    if (!upath || !plen)
        return -A20_ERR_INVALID_ARGUMENT;
    if (copy_path_from_user(path, upath, plen) < 0)
        return -A20_ERR_FAULT;
    /* Success does not return: the caller becomes the checkpointed task. */
    return ckpt_native_status(proc_ckpt_restore_path(path));
}
