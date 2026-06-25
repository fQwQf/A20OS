#include "proc/proc_internal.h"
#include "proc/signal.h"

int a20_wait_task_pid(task_t *task) { return task ? task->pid : -1; }
int a20_wait_task_tgid(task_t *task) { return task ? task->tgid : -1; }
int a20_wait_task_ppid(task_t *task) { return task ? task->ppid : -1; }
int a20_wait_task_pgid(task_t *task) { return task ? task->pgid : -1; }
task_t *a20_wait_task_parent(task_t *task) { return task ? task->parent : NULL; }
int a20_wait_task_state_acquire(task_t *task)
{
    return task ? __atomic_load_n((int *)&task->state, __ATOMIC_ACQUIRE) : PROC_UNUSED;
}
void a20_wait_task_set_state(task_t *task, int state)
{
    if (task)
        task->state = (proc_state_t)state;
}
int a20_wait_task_exit_code_acquire(task_t *task)
{
    return task ? __atomic_load_n(&task->exit_code, __ATOMIC_ACQUIRE) : 0;
}
uint64_t a20_wait_task_total_time(task_t *task)
{
    return task ? task->total_time : 0;
}
void a20_wait_task_add_child_utime(task_t *task, uint64_t delta)
{
    if (task)
        task->child_utime += delta;
}
int a20_wait_task_waiting_for_child(task_t *task)
{
    return task ? task->waiting_for_child : 0;
}
void a20_wait_task_set_waiting_for_child(task_t *task, int waiting)
{
    if (task)
        task->waiting_for_child = waiting;
}
int a20_wait_task_is_clone_child(task_t *task)
{
    if (!task)
        return 0;
    if (task->clone_flags & CLONE_THREAD)
        return 1;
    return task->exit_signal != SIGCHLD;
}
