/*
 * MM-04 deterministic allocation-failure injection.
 *
 * See kernel/include/mm/failinject.h for the contract.  The whole file
 * compiles away when CONFIG_MM_FAIL_STAGE is unset; only the build-only
 * images that arm a stage link it.
 */
#include "mm/failinject.h"

#ifdef CONFIG_MM_FAIL_STAGE

#include "core/klog.h"

#if CONFIG_MM_FAIL_STAGE < MM_FAIL_STAGE_PT_NODE || \
    CONFIG_MM_FAIL_STAGE > MM_FAIL_STAGE_SWAP_SLOT
#error "CONFIG_MM_FAIL_STAGE must name one MM_FAIL_STAGE_* site"
#endif

/* Fire once per boot: the workload reaches its stage many times, but one
 * deterministic failure is what the rollback assertion needs.  The exchange
 * also stops a concurrent second caller from firing the same stage twice. */
static int mm_fail_injected;
static int mm_fail_scope;

static int mm_fail_fire(int stage)
{
    if (stage != CONFIG_MM_FAIL_STAGE)
        return 0;
    if (__atomic_exchange_n(&mm_fail_injected, 1, __ATOMIC_ACQ_REL))
        return 0;
    klog_write("MM_FAILINJECT: stage=%d fired\n", stage);
    return 1;
}

int mm_fail_inject_fire(int stage)
{
    return mm_fail_fire(stage);
}

int mm_fail_inject_fire_scoped(int stage)
{
    if (!mm_fail_scope)
        return 0;
    return mm_fail_fire(stage);
}

void mm_fail_inject_scope(int on)
{
    mm_fail_scope = on;
}

#endif /* CONFIG_MM_FAIL_STAGE */
