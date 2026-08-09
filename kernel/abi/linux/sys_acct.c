#include "syscall_impl.h"

#include "proc/acct.h"

int64_t sys_acct(const char *filename)
{
    if (!filename)
        return acct_disable();

    char path[MAX_PATH_LEN];
    if (user_strncpy(path, filename, sizeof(path)) < 0)
        return -EFAULT;
    return acct_enable(path);
}
