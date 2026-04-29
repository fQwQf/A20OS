#ifndef _CORE_CPU_H
#define _CORE_CPU_H

#include "core/arch.h"

#ifndef CONFIG_NR_CPUS
#define CONFIG_NR_CPUS 1
#endif

/*
 * Shared CPU-local helpers.
 *
 * The current build still starts one CPU, but shared kernel code should go
 * through this wrapper instead of assuming CPU 0. When SMP bringup grows real
 * secondary CPU startup, only the arch_current_cpu_id() backend and scheduler
 * policy need to change.
 */
static inline unsigned cpu_current_id(void)
{
    unsigned id = arch_current_cpu_id();
    return id < CONFIG_NR_CPUS ? id : 0;
}

#endif /* _CORE_CPU_H */
