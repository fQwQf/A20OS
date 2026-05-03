#ifndef _PROC_INTERNAL_H
#define _PROC_INTERNAL_H

#include "proc/proc.h"
#include "core/lock.h"

#define SCHED_LEVELS 8
#define SCHED_NO_DEADLINE (~0ULL)

extern spinlock_t proc_lock;

task_t *proc_idle_task(void);
task_t *proc_first_task_locked(void);
task_t *proc_next_task_locked(task_t *t);
void proc_unlink_task_locked(task_t *t);
task_t *proc_set_current(task_t *next);
uint64_t *proc_kernel_pgdir_shared(void);

void proc_pid_init(void);
int proc_pid_alloc(void);
void proc_pid_register(task_t *t);
void proc_pid_unregister(task_t *t);
int proc_pid_next_value(void);

void proc_task_init_common(task_t *t, task_t *parent);
void proc_task_release_resources(task_t *t);
void proc_destroy_task(task_t *t);
task_t *proc_alloc_task_slot(void);

void proc_sched_runq_init(void);
unsigned proc_sched_select_cpu_locked(task_t *t);
void proc_sched_kick_cpu(unsigned cpu);
void proc_runq_enqueue_locked(task_t *t);
void proc_runq_remove_locked(task_t *t);
task_t *proc_runq_pick_locked(void);
int proc_has_runnable_locked(void);

#endif
