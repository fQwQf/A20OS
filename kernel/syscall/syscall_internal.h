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

#endif /* _SYSCALL_INTERNAL_H */
