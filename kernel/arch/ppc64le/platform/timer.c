#ifdef CONFIG_PPC64LE

#include "core/defs.h"
#include "core/timer.h"
#include "firmware.h"

void timer_init(void)
{
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks)
{
    firmware_set_timer(timer_get_ticks() + ticks);
}

uint64_t timer_get_ticks(void)
{
    return arch_read_cycle();
}

void timer_irq_tick(void)
{
}

void timer_enable(void)
{
}

void timer_disable(void)
{
}

#endif /* CONFIG_PPC64LE */
