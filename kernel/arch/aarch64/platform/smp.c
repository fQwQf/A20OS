#ifdef CONFIG_AARCH64

#include "core/smp.h"
#include "core/cpu.h"

void smp_send_reschedule(unsigned cpu)
{
    (void)cpu;
}

void smp_init(void)
{
}

void smp_boot_secondaries(void)
{
}

void smp_secondary_init(unsigned cpu_id)
{
    (void)cpu_id;
}

#endif /* CONFIG_AARCH64 */
