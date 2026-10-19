#include "proc/task_construction.h"
#include "core/errno.h"
#ifdef CONFIG_TASK_CONSTRUCTION_TEST
#include "core/lock.h"
#endif

#ifdef CONFIG_TASK_CONSTRUCTION_TEST
static uintptr_t g_task_construction_test_owner;
static unsigned g_task_construction_test_stage;
static spinlock_t g_task_construction_test_lock = SPINLOCK_INIT;

int proc_task_construction_test_arm(void *owner, unsigned stage)
{
    if (!owner || stage != TASK_CONSTRUCTION_STAGE_MM)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_task_construction_test_lock);
    if (g_task_construction_test_stage != 0) {
        spin_unlock_irqrestore(&g_task_construction_test_lock, flags);
        return -EBUSY;
    }
    g_task_construction_test_owner = (uintptr_t)owner;
    g_task_construction_test_stage = stage;
    spin_unlock_irqrestore(&g_task_construction_test_lock, flags);
    return 0;
}

int proc_task_construction_test_fail_after(void *owner, unsigned stage)
{
    uint64_t flags = spin_lock_irqsave(&g_task_construction_test_lock);
    int fail = g_task_construction_test_owner == (uintptr_t)owner &&
               g_task_construction_test_stage == stage;
    if (fail) {
        g_task_construction_test_owner = 0;
        g_task_construction_test_stage = 0;
    }
    spin_unlock_irqrestore(&g_task_construction_test_lock, flags);
    return fail ? -ENOMEM : 0;
}

void proc_task_construction_test_cancel(void *owner)
{
    uint64_t flags = spin_lock_irqsave(&g_task_construction_test_lock);
    if (g_task_construction_test_owner == (uintptr_t)owner) {
        g_task_construction_test_owner = 0;
        g_task_construction_test_stage = 0;
    }
    spin_unlock_irqrestore(&g_task_construction_test_lock, flags);
}
#endif
