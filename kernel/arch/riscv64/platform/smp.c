#ifdef CONFIG_RISCV64

#include "core/smp.h"
#include "core/cpu.h"
#include "core/defs.h"
#include "core/trap.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "proc/proc.h"
#include "firmware.h"

volatile uint64_t riscv64_smp_release;
static volatile unsigned riscv64_cpu_online[CONFIG_NR_CPUS];

static unsigned riscv64_logical_to_hart(unsigned cpu)
{
    extern uint64_t __boot_hart_id;
    return (cpu + (unsigned)__boot_hart_id) % CONFIG_NR_CPUS;
}

unsigned arch_cpu_hart_id(unsigned cpu_id)
{
    return riscv64_logical_to_hart(cpu_id);
}

unsigned arch_current_cpu_id(void)
{
    task_t *current = (task_t *)arch_get_task_pointer();
    if (current && arch_is_kernel_address(current) &&
        current->cpu_id < CONFIG_NR_CPUS)
        return current->cpu_id;

    extern uint64_t __boot_hart_id;
    return (unsigned)__boot_hart_id;
}

void smp_send_reschedule(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return;
    sbi_send_ipi(1UL << riscv64_logical_to_hart(cpu), 0);
}

void smp_init(void)
{
    smp_core_init();
    riscv64_smp_release = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        riscv64_cpu_online[cpu] = 0;
    riscv64_cpu_online[0] = 1;
}

void smp_boot_secondaries(void)
{
    if (CONFIG_NR_CPUS <= 1)
        return;

    __atomic_store_n(&riscv64_smp_release, 1, __ATOMIC_RELEASE);
    extern char _start[];
    uint64_t start_pa = (uint64_t)(uintptr_t)_start - PAGE_OFFSET;
    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++) {
        int64_t err = sbi_hart_start(riscv64_logical_to_hart(cpu), start_pa, 0);
        if (err && err != -4)
            printf("[SMP] hart start failed: cpu=%u err=%ld\n", cpu, (long)err);
    }

    for (unsigned cpu = 1; cpu < CONFIG_NR_CPUS; cpu++)
        while (!__atomic_load_n(&riscv64_cpu_online[cpu], __ATOMIC_ACQUIRE))
            cpu_relax();

    printf("[SMP] %u CPUs online\n", CONFIG_NR_CPUS);
}

void smp_secondary_init(unsigned cpu_id)
{
    proc_init_secondary(cpu_id);
    trap_init();
    timer_init();
    smp_cpu_mark_online(cpu_id);
    __atomic_store_n(&riscv64_cpu_online[cpu_id], 1, __ATOMIC_RELEASE);
}

void riscv64_secondary_entry(unsigned cpu_id)
{
    smp_secondary_init(cpu_id);
    arch_local_irq_enable();
    idle_loop();
}

#endif /* CONFIG_RISCV64 */
