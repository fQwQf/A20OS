/*
 * musl A20 posix_spawn — maps to A20 task_spawn.
 */
#include <stdint.h>
#include <errno.h>
#include "syscall.h"

struct spawn_file_actions;
struct spawn_attr;

int __a20_posix_spawn(int *pid, const char *path,
                       const struct spawn_file_actions *fa,
                       const struct spawn_attr *attr,
                       char **argv, char **envp)
{
    (void)fa; (void)attr;
    long r = __syscall6(0x0201, (long)path, (long)argv, (long)envp, 0, 0, (long)pid);
    if (r < 0) return -(int)r;
    return 0;
}
