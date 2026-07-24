#ifndef _A20_SYSTEM_H
#define _A20_SYSTEM_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline a20_status_t a20_system_info(a20_system_info_t *info)
{
    info->size = sizeof(*info);
    info->struct_version = 2;
    return a20_syscall6(A20_SYS_system_info, (uint64_t)info, 0, 0, 0, 0, 0);
}

#endif
