#ifndef A20_PROC_TASK_EXIT_H
#define A20_PROC_TASK_EXIT_H

#include "core/types.h"

typedef void (*task_exit_stage_fn)(void *context);
typedef task_exit_stage_fn task_release_stage_fn;

typedef struct task_exit_ops {
    void *context;
    task_exit_stage_fn begin;
    task_exit_stage_fn clear_tid;
    task_exit_stage_fn robust_list;
    task_exit_stage_fn release_locks;
    task_exit_stage_fn account;
    task_exit_stage_fn close_files;
    task_exit_stage_fn release_mm;
    task_exit_stage_fn debug_stop;
    task_exit_stage_fn resolve_parent;
    task_exit_stage_fn publish_zombie;
    task_exit_stage_fn parent_bookkeeping;
    task_exit_stage_fn task_services;
    task_exit_stage_fn reparent_children;
    task_exit_stage_fn tracer_detach;
    task_exit_stage_fn notify_parent;
    task_exit_stage_fn schedule;
} task_exit_ops_t;

typedef struct task_release_ops {
    void *context;
    task_release_stage_fn cgroup;
    task_release_stage_fn udriver;
    task_release_stage_fn process_locks;
    task_release_stage_fn files;
    task_release_stage_fn mm;
    task_release_stage_fn arch;
    task_release_stage_fn keyring;
    task_release_stage_fn landlock;
    task_release_stage_fn envelope;
    task_release_stage_fn thread_signal_info;
    task_release_stage_fn signal_state;
    task_release_stage_fn scratch;
    task_release_stage_fn handle_table;
    task_release_stage_fn nommu_vfork;
    task_release_stage_fn kernel_stack;
    task_release_stage_fn thread_leader;
    task_release_stage_fn task_object;
    task_release_stage_fn pid;
    task_release_stage_fn free_task;
} task_release_ops_t;

void a20_task_exit_run(const task_exit_ops_t *ops);
void a20_task_release_run(const task_release_ops_t *ops);
int a20_task_ref_dec(volatile int *value);

#endif
