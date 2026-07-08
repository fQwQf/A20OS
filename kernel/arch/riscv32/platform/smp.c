#ifdef CONFIG_RISCV32

#include "core/smp.h"
#include "firmware.h"

void smp_send_reschedule(unsigned cpu) {
    if (cpu >= CONFIG_NR_CPUS)
        return;
    sbi_send_ipi(1U << cpu);
}

void smp_init(void) {
}

void smp_boot_secondaries(void) {
}

void smp_secondary_init(unsigned cpu_id) {
    (void)cpu_id;
}

#endif /* CONFIG_RISCV32 */
