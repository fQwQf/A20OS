#ifdef CONFIG_AARCH64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "proc/proc.h"

uint64_t arch_smp_boot_hw_id(void)
{
    uint64_t mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xFFFFFFUL;
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char aarch64_secondary_start[];
    return (uintptr_t)aarch64_secondary_start - PAGE_OFFSET;
}

void aarch64_secondary_entry(unsigned cpu_id)
{
    if (cpu_id == 0 || cpu_id >= CONFIG_NR_CPUS)
        arch_halt();
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_AARCH64 */
