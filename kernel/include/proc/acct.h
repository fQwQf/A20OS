#ifndef _PROC_ACCT_H
#define _PROC_ACCT_H

/*
 * A20OS process accounting subsystem (ABI-independent).
 *
 * Implements the Linux acct(2) model: a single accounting file per kernel,
 * opened by acct(path).  When a task exits, an accounting record is appended.
 * acct(NULL) disables accounting.
 *
 * The ABI layer (kernel/abi/linux/sys_acct.c) copies the user pathname and
 * delegates here.  This file owns the accounting-file global fd and the
 * record format; it never touches user pointers directly.
 */

#include "core/types.h"

struct task_t;

/* Enable accounting on @path (a kernel NUL-terminated pathname), or disable
 * accounting when @path is NULL.  Returns 0 on success or a negative errno. */
int acct_enable(const char *path);
int acct_disable(void);

/* Called from proc_exit(); appends an accounting record for @t if accounting
 * is active.  Must never block on user code and must tolerate being called
 * for any task. */
void acct_task_exit(struct task_t *t);

#endif /* _PROC_ACCT_H */
