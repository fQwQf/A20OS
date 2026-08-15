#ifdef CONFIG_LOONGARCH32

#include "core/smp.h"
#include "core/cpu.h"

/* NaiLoong Core is a single-core target without IOCSR IPIs.  Keep the arch
 * SMP hooks minimal so a multi-CPU kernel build still links; the board keeps
 * .smp = NULL and the common core degrades to BSP-only. */

uint64_t arch_smp_boot_hw_id(void)
{
    return arch_current_cpu_id();
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char _start[];
    return (uintptr_t)_start - PAGE_OFFSET;
}

#endif /* CONFIG_LOONGARCH32 */
