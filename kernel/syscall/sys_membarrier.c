#include "syscall_internal.h"

int64_t sys_membarrier(int cmd, unsigned flags, int cpu_id)
{
    (void)cpu_id;
    if (flags) return -EINVAL;
    if (cmd == 0) return 1;
    return 0;
}
