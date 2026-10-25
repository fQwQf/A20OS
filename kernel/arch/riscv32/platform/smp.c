#ifdef CONFIG_RISCV32
#include "core/smp.h"
#include "core/defs.h"
#include "proc/proc.h"
#include "platform.h"
unsigned arch_current_cpu_id(void)
{
    task_t *task = arch_get_task_pointer();
    if (task && arch_is_kernel_address(task) && task->cpu_id < CONFIG_NR_CPUS)
        return task->cpu_id;
    return 0;
}
uint64_t arch_smp_boot_hw_id(void)
{
    extern uint32_t __boot_hart_id;
    return __boot_hart_id;
}
uintptr_t arch_smp_secondary_entry_pa(void)
{
    extern char riscv32_secondary_start[];
    return (uintptr_t)riscv32_secondary_start;
}
void riscv32_secondary_entry(unsigned cpu_id)
{
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}
#endif
