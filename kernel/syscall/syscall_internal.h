#ifndef _SYSCALL_INTERNAL_H
#define _SYSCALL_INTERNAL_H

#include "core/consts.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/types.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "proc/proc.h"

extern int syscall_sig_diag_count;
extern int syscall_sleep_diag_count;

int syscall_path_at(int dirfd, const char *path, char *out, size_t outsz);

#include "abi/linux/syscall_entry.h"

void syscall_trace_enter(task_t *task, const linux_syscall_entry_t *entry,
                         const linux_syscall_args_t *args);
void syscall_trace_exit(task_t *task, const linux_syscall_entry_t *entry,
                        int64_t ret);
void syscall_trace_slow_scanner(void);

#endif /* _SYSCALL_INTERNAL_H */
