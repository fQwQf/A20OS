#ifdef CONFIG_RISCV32

#include "core/smp.h"
#include "platform.h"

uint64_t arch_smp_boot_hw_id(void)
{
    extern uint32_t __boot_hart_id;
    return __boot_hart_id;
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char _start[];
    return (uintptr_t)_start - PAGE_OFFSET;
}

#endif /* CONFIG_RISCV32 */
