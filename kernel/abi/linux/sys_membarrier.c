#include "syscall_impl.h"

#include "core/smp.h"
#include "mm/vm.h"
#include "proc/proc.h"

/* Linux membarrier(2) commands (include/uapi/linux/membarrier.h). */
#define MEMBARRIER_CMD_QUERY                         0
#define MEMBARRIER_CMD_GLOBAL                         (1U << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED                (1U << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED       (1U << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED               (1U << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED      (1U << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE     (1U << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1U << 6)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ          (1U << 7)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ (1U << 8)

/* A20OS supports the full barrier set.  RSEQ registration currently only
 * records the rseq area (no per-thread migration abort yet), so the RSEQ
 * membarrier commands are advertised as registration-only no-ops. */
#define MEMBARRIER_SUPPORTED \
    (MEMBARRIER_CMD_GLOBAL | \
     MEMBARRIER_CMD_GLOBAL_EXPEDITED | \
     MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED | \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE | \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE | \
     MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ | \
     MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)

int64_t sys_membarrier(int cmd, unsigned flags, int cpu_id)
{
    (void)cpu_id;
    if (flags)
        return -EINVAL;

    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    switch (cmd) {
    case MEMBARRIER_CMD_QUERY:
        return (int64_t)MEMBARRIER_SUPPORTED;
    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
        return smp_membarrier_sync_all();
    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
        /* Registration is a per-mm opt-in; global barriers never require
         * registration.  Record nothing and succeed. */
        return 0;
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
        if (!t->mm || !t->mm->membarrier_registered)
            return -EPERM;
        return smp_membarrier_sync_all();
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
        if (!t->mm)
            return -ESRCH;
        t->mm->membarrier_registered = 1;
        return 0;
    case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
        if (!t->mm || !t->mm->membarrier_registered)
            return -EPERM;
        /* The rseq area is recorded per-thread; the expedited RSEQ barrier
         * requires the per-mm registration, then a barrier like any private
         * expedited request. */
        return smp_membarrier_sync_all();
    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ:
        if (!t->mm)
            return -ESRCH;
        t->mm->membarrier_registered = 1;
        return 0;
    default:
        return -EINVAL;
    }
}
