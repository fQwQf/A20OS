#include "proc/task_lifecycle.h"
#include "proc/proc.h"
#include "core/errno.h"

int proc_task_lifecycle_advance(struct task_t *task, uint32_t event)
{
    if (!task)
        return -EINVAL;
    extern int a20_task_lifecycle_advance(uint32_t *phase, uint32_t event);
    return a20_task_lifecycle_advance(&task->lifecycle_phase, event);
}
