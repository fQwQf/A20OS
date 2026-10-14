#ifndef A20_PROC_TASK_LIFECYCLE_H
#define A20_PROC_TASK_LIFECYCLE_H

#include "core/types.h"

struct task_t;

enum {
    PROC_TASK_PHASE_ALLOCATED = 1,
    PROC_TASK_PHASE_INITIALIZING = 2,
    PROC_TASK_PHASE_PUBLISHED = 3,
    PROC_TASK_PHASE_RUNNABLE = 4,
    PROC_TASK_PHASE_DESTROYING = 5,
};

enum {
    PROC_TASK_EVENT_BEGIN_INIT = 1,
    PROC_TASK_EVENT_PUBLISH = 2,
    PROC_TASK_EVENT_READY = 3,
    PROC_TASK_EVENT_DESTROY = 4,
};

/* Caller serializes phase changes with the task's existing lifecycle lock:
 * unpublished construction has a single owner; published tasks use
 * tasklist_lock -> park_lock for publication/destruction and park_lock for
 * the scheduler-ready commit. */
int proc_task_lifecycle_advance(struct task_t *task, uint32_t event);

#endif
