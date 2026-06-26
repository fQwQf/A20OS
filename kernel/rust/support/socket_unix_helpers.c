#include "net/socket_internal.h"

const char *a20_unix_current_cwd(void)
{
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    return cwd;
}

int a20_unix_vfs_stat(const char *path, kstat_t *st)
{
    return vfs_stat(path, st);
}

int a20_unix_vfs_open(const char *path, int flags, int mode)
{
    return vfs_open(path, flags, mode);
}

int a20_unix_vfs_close(int fd)
{
    return vfs_close(fd);
}
