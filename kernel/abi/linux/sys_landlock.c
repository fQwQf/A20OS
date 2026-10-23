#include "syscall_impl.h"

#include "ipc/landlock.h"

int64_t sys_landlock_create_ruleset(const void *attr, size_t size,
                                    unsigned flags)
{
    if (flags || size < sizeof(uint64_t))
        return -EINVAL;
    if (size > PAGE_SIZE)
        return -E2BIG;
    if (!attr)
        return -EFAULT;
    uint64_t handled = 0;
    if (copy_from_user(&handled, attr, sizeof(handled)) < 0)
        return -EFAULT;
    /* A newer caller may request network/scoping restrictions in the tail.
     * Never install a weaker filesystem-only ruleset and report success. */
    for (size_t offset = sizeof(handled); offset < size;) {
        uint8_t tail[64];
        size_t count = size - offset;
        if (count > sizeof(tail))
            count = sizeof(tail);
        if (copy_from_user(tail, (const char *)attr + offset, count) < 0)
            return -EFAULT;
        for (size_t i = 0; i < count; i++)
            if (tail[i])
                return -E2BIG;
        offset += count;
    }
    if (!handled)
        return -ENOMSG;
    /* Only open's READ_FILE/WRITE_FILE checks are wired into the VFS.
     * Accepting EXECUTE, directory mutation or ioctl bits would promise
     * enforcement that those entry points do not provide. */
    if (handled & ~(LANDLOCK_ACCESS_FS_READ_FILE |
                    LANDLOCK_ACCESS_FS_WRITE_FILE))
        return -EOPNOTSUPP;
    return landlock_create_ruleset(handled);
}

int64_t sys_landlock_add_rule(int ruleset_fd, int rule_type,
                              const void *attr, unsigned flags)
{
    return landlock_add_rule(ruleset_fd, rule_type, attr, flags);
}

int64_t sys_landlock_restrict_self(int ruleset_fd, unsigned flags)
{
    return landlock_restrict_self(ruleset_fd, flags);
}
