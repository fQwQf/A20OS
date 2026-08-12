#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "core/string.h"
#include "ipc/landlock.h"
#include "proc/proc.h"

/*
 * LSM introspection syscalls: lsm_get_self_attr, lsm_set_self_attr,
 * lsm_list_modules.
 *
 * A20OS has a single LSM (Landlock).  get_self_attr reports the Landlock
 * ruleset fds the process has restricted itself with; set_self_attr is only
 * meaningful at first-restriction time and is handled by
 * landlock_restrict_self; list_modules reports "landlock" plus the
 * "capability" pseudo-module that is always present.
 */

#define LSM_ATTR_CURRENT   0
#define LSM_ATTR_PREV      1
#define LSM_ATTR_EARLIEST  2

#define LSM_ID_CAPABILITY  1
#define LSM_ID_LANDLOCK    34

int64_t sys_lsm_get_self_attr(unsigned int attr, void *ctx, size_t *size,
                              unsigned int flags)
{
    (void)flags;
    if (!ctx || !size)
        return -EFAULT;
    uint64_t usize = 0;
    if (copy_from_user(&usize, size, sizeof(usize)) < 0)
        return -EFAULT;

    if (attr == LSM_ATTR_CURRENT) {
        task_t *t = proc_current();
        /* Report the Landlock ruleset handle if the process is restricted. */
        if (t && t->landlock_rulesets) {
            if (usize < 16)
                return -ERANGE;
            uint64_t lsm_id = LSM_ID_LANDLOCK;
            uint64_t flags_out = 0;
            if (copy_to_user(ctx, &lsm_id, sizeof(lsm_id)) < 0 ||
                copy_to_user((char *)ctx + 8, &flags_out, sizeof(flags_out)) < 0)
                return -EFAULT;
            usize = 16;
            if (copy_to_user(size, &usize, sizeof(usize)) < 0)
                return -EFAULT;
            return 0;
        }
        /* No Landlock restriction: nothing to report. */
        usize = 0;
        if (copy_to_user(size, &usize, sizeof(usize)) < 0)
            return -EFAULT;
        return 0;
    }
    return -EINVAL;
}

int64_t sys_lsm_set_self_attr(unsigned int attr, const void *ctx,
                              size_t size, unsigned int flags)
{
    (void)attr;
    (void)ctx;
    (void)size;
    (void)flags;
    /* A20OS applies Landlock rulesets through landlock_restrict_self; the
     * generic set_self_attr has no additional state to set. */
    return -EOPNOTSUPP;
}

int64_t sys_lsm_list_modules(uint64_t *ids, size_t *size, unsigned int flags)
{
    (void)flags;
    if (!size)
        return -EFAULT;
    uint64_t usize = 0;
    if (copy_from_user(&usize, size, sizeof(usize)) < 0)
        return -EFAULT;

    uint64_t mods[2] = { LSM_ID_CAPABILITY, LSM_ID_LANDLOCK };
    uint64_t need = 2 * sizeof(uint64_t);
    if (usize < need) {
        if (copy_to_user(size, &need, sizeof(need)) < 0)
            return -EFAULT;
        return -ERANGE;
    }
    if (ids && copy_to_user(ids, mods, need) < 0)
        return -EFAULT;
    if (copy_to_user(size, &need, sizeof(need)) < 0)
        return -EFAULT;
    return 2;
}
