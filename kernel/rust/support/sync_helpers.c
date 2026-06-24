#include "proc/proc.h"

void *a20_proc_current(void)
{
    return proc_current();
}

void a20_proc_make_ready(void *t)
{
    if (t)
        proc_make_ready((task_t *)t);
}

void a20_sched(void)
{
    sched();
}

int a20_task_state(void *t)
{
    if (!t)
        return 0;
    return (int)((task_t *)t)->state;
}

void a20_task_set_state(void *t, int state)
{
    if (t)
        ((task_t *)t)->state = (proc_state_t)state;
}
