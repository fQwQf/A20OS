#ifdef CONFIG_PPC64LE

#include "core/smp.h"
#include "platform.h"

uint64_t arch_smp_boot_hw_id(void)
{
    return arch_current_cpu_id();
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char _start[];
    return (uintptr_t)_start - PAGE_OFFSET;
}

#endif /* CONFIG_PPC64LE */
