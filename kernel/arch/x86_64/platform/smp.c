#ifdef CONFIG_X86_64

#include "core/smp.h"

void smp_send_reschedule(unsigned cpu)
{
    (void)cpu;
}

void smp_init(void)
{
    smp_core_init();
}

void smp_boot_secondaries(void)
{
}

void smp_secondary_init(unsigned cpu_id)
{
    (void)cpu_id;
}

#endif
