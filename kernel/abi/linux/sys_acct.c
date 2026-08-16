#include "syscall_impl.h"

#include "proc/acct.h"

int64_t sys_acct(const char *filename)
{
    if (!filename)
        return acct_disable();

    char path[MAX_PATH_LEN];
    long pr0 = user_path_strncpy(path, filename, sizeof(path));
    if (pr0 < 0)
        return pr0;
    return acct_enable(path);
}
