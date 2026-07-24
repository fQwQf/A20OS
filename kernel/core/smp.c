#include "core/smp.h"

static uint32_t online_mask = 1U;

void smp_core_init(void)
{
    __atomic_store_n(&online_mask, 1U, __ATOMIC_RELEASE);
}

void smp_cpu_mark_online(unsigned cpu)
{
    if (cpu < CONFIG_NR_CPUS && cpu < 32)
        __atomic_fetch_or(&online_mask, 1U << cpu, __ATOMIC_RELEASE);
}

unsigned smp_configured_cpu_count(void)
{
    return CONFIG_NR_CPUS;
}

uint32_t smp_online_cpu_mask(void)
{
    return __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
}

unsigned smp_online_cpu_count(void)
{
    return (unsigned)__builtin_popcount(smp_online_cpu_mask());
}

int smp_cpu_is_online(unsigned cpu)
{
    return cpu < 32 && (smp_online_cpu_mask() & (1U << cpu)) != 0;
}
