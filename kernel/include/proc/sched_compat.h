#ifndef _PROC_SCHED_COMPAT_H
#define _PROC_SCHED_COMPAT_H

/*
 * ioprio_get/ioprio_set(2) and pkey_alloc/pkey_free/pkey_mprotect(2)
 * compatibility helpers.
 *
 * ioprio is stored per task and validated against the Linux encoding;
 * it does not yet feed the block scheduler.  Protection keys are a per-task
 * bitmap of 16 slots; pkey_mprotect accepts a key whose bit is allocated and
 * otherwise behaves like mprotect (the hardware key registers are not
 * programmed, matching the single-user model).
 */

#include "core/types.h"

struct task_t;

/* ioprio */
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_CLASS_MASK  0x7
#define IOPRIO_DATA_MASK   0xff
#define IOPRIO_CLASS_NONE  0
#define IOPRIO_CLASS_RT    1
#define IOPRIO_CLASS_BE    2
#define IOPRIO_CLASS_IDLE  3

#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_WHO_PGRP    2
#define IOPRIO_WHO_USER    3

int  ioprio_set_task(struct task_t *caller, int which, int who, int ioprio);
int  ioprio_get_task(struct task_t *caller, int which, int who);

/* pkeys */
#define PKEY_DISABLE_ACCESS 0x1
#define PKEY_DISABLE_WRITE  0x2
#define PKEY_MAX_KEYS       16

int  pkey_alloc(struct task_t *t, unsigned flags);
int  pkey_free(struct task_t *t, int key);
int  pkey_valid(struct task_t *t, int key);

#endif /* _PROC_SCHED_COMPAT_H */
