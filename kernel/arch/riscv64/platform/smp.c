#ifdef CONFIG_RISCV64

#include "core/smp.h"
#include "core/defs.h"
#include "proc/proc.h"

volatile uint64_t riscv64_smp_release;
volatile uint32_t riscv64_smp_context_valid;
uint64_t riscv64_smp_context_hw_id[CONFIG_NR_CPUS];

unsigned arch_cpu_hart_id(unsigned cpu_id)
{
    const smp_cpu_desc_t *cpu = smp_cpu_desc(cpu_id);
    if (cpu)
        return (unsigned)cpu->hw_id;

    extern uint64_t __boot_hart_id;
    return (unsigned)__boot_hart_id;
}

unsigned arch_current_cpu_id(void)
{
    task_t *current = (task_t *)arch_get_task_pointer();
    if (current && arch_is_kernel_address(current) &&
        current->cpu_id < CONFIG_NR_CPUS)
        return current->cpu_id;

    extern uint64_t __boot_hart_id;
    unsigned logical_id;
    if (!smp_hw_to_logical(__boot_hart_id, &logical_id))
        return logical_id;
    return 0;
}

uint64_t arch_smp_boot_hw_id(void)
{
    extern uint64_t __boot_hart_id;
    return __boot_hart_id;
}

uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char _start[];
    return (uintptr_t)_start - PAGE_OFFSET;
}

int arch_smp_secondary_prepare(const smp_cpu_desc_t *cpu)
{
    if (!cpu || cpu->logical_id == 0 || cpu->logical_id >= CONFIG_NR_CPUS)
        return -1;
    riscv64_smp_context_hw_id[cpu->logical_id] = cpu->hw_id;
    __atomic_fetch_or(&riscv64_smp_context_valid,
                      1U << cpu->logical_id, __ATOMIC_RELEASE);
    __atomic_store_n(&riscv64_smp_release, 1, __ATOMIC_RELEASE);
    return 0;
}

void riscv64_secondary_entry(unsigned cpu_id)
{
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_RISCV64 */
