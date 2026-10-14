#include "proc/task_lifecycle.h"
#include "proc/proc.h"
#include "core/errno.h"

#ifndef CONFIG_TASK_LIFECYCLE_RUST
static int task_lifecycle_advance_c(uint32_t *phase, uint32_t event)
{
    uint32_t current = *phase;
    switch (event) {
    case PROC_TASK_EVENT_BEGIN_INIT:
        if (current == PROC_TASK_PHASE_ALLOCATED) {
            *phase = PROC_TASK_PHASE_INITIALIZING;
            return 0;
        }
        break;
    case PROC_TASK_EVENT_PUBLISH:
        if (current == PROC_TASK_PHASE_INITIALIZING) {
            *phase = PROC_TASK_PHASE_PUBLISHED;
            return 0;
        }
        break;
    case PROC_TASK_EVENT_READY:
        if (current == PROC_TASK_PHASE_PUBLISHED) {
            *phase = PROC_TASK_PHASE_RUNNABLE;
            return 0;
        }
        if (current == PROC_TASK_PHASE_RUNNABLE)
            return 0;
        break;
    case PROC_TASK_EVENT_DESTROY:
        if (current == PROC_TASK_PHASE_ALLOCATED ||
            current == PROC_TASK_PHASE_INITIALIZING ||
            current == PROC_TASK_PHASE_PUBLISHED ||
            current == PROC_TASK_PHASE_RUNNABLE) {
            *phase = PROC_TASK_PHASE_DESTROYING;
            return 0;
        }
        break;
    default:
        break;
    }
    return -EINVAL;
}
#endif

int proc_task_lifecycle_advance(struct task_t *task, uint32_t event)
{
    if (!task)
        return -EINVAL;
#ifdef CONFIG_TASK_LIFECYCLE_RUST
    extern int a20_task_lifecycle_advance(uint32_t *phase, uint32_t event);
    return a20_task_lifecycle_advance(&task->lifecycle_phase, event);
#else
    return task_lifecycle_advance_c(&task->lifecycle_phase, event);
#endif
}
