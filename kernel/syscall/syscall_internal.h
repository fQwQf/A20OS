#ifndef _SYSCALL_INTERNAL_H
#define _SYSCALL_INTERNAL_H

#include "sys/syscall.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/timer.h"
#include "drv/uart.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "net/socket.h"

extern int syscall_sig_diag_count;
extern int syscall_sleep_diag_count;

int64_t syscall_get_global_fd(int fd);
int syscall_alloc_local_fd(task_t *t, int gfd);
int syscall_alloc_local_fd_with_flags(task_t *t, int gfd, int flags);

#endif /* _SYSCALL_INTERNAL_H */
