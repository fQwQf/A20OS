#ifndef _CORE_CPU_H
#define _CORE_CPU_H

#include "core/arch.h"

#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

/*
 * Shared CPU-local helpers.
 *
 * The architecture supplies a dense logical ID for the physical CPU running
 * this instruction. The ID must remain CPU-local when tasks migrate, and
 * agree with the SMP topology and per-CPU scheduler/current slots.
 */
static inline unsigned cpu_current_id(void)
{
    unsigned id = arch_current_cpu_id();
    return id < CONFIG_NR_CPUS ? id : 0;
}

static inline void cpu_relax(void)
{
    arch_cpu_relax();
}

#endif /* _CORE_CPU_H */
