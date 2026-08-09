#include "syscall_impl.h"

#include "ipc/landlock.h"

int64_t sys_landlock_create_ruleset(const void *attr, size_t size,
                                    unsigned flags)
{
    (void)size;
    if (!attr)
        return -EFAULT;
    uint64_t handled = 0;
    if (copy_from_user(&handled, attr, sizeof(handled)) < 0)
        return -EFAULT;
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
