#ifdef CONFIG_ARM32
#include "core/smp.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "platform.h"
uint64_t arch_smp_boot_hw_id(void) { return arch_current_cpu_id(); }
uintptr_t arch_smp_secondary_entry_pa(void)
{
    /* Entry is in the physical .text.entry section, already identity-linked. */
    extern char arm32_secondary_start[];
    return (uintptr_t)arm32_secondary_start;
}
int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu)
{
    (void)cpu;
    arch_dma_sync_for_device((void *)((uintptr_t)boot_pgdir + PAGE_OFFSET), 16384);
    return 0;
}
void arm32_secondary_entry(unsigned cpu)
{
    smp_secondary_init(cpu);
    arch_local_irq_enable();
    idle_loop();
}
#endif
