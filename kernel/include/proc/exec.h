#ifndef _PROC_EXEC_H
#define _PROC_EXEC_H

#include "core/types.h"

/* Kernel-owned startup bytes and an optional handle retirement list. The
 * process core transports the bytes without interpreting libc fd state. */
typedef struct proc_exec_bootstrap {
    const void *data;
    size_t size;
    const uint32_t *close_handles;
    uint32_t close_count;
} proc_exec_bootstrap_t;

int proc_exec_with_bootstrap(const char *path, char *const argv[],
                             char *const envp[],
                             const proc_exec_bootstrap_t *bootstrap);

#endif
