#ifdef CONFIG_PPC64LE

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "core/progress.h"
#include "drivers/core/driver_hwapi.h"

static void handle_timer_irq(int from_user)
{
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    if (!from_user)
        return;
    task_t *cur = proc_current();
    if (cur)
        cur->total_time++;
}

void trap_init(void)
{
    arch_write_tvec((uint64_t)__trap_from_kernel);
}

void arch_handle_irq(uint64_t irq, int from_user)
{
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }
    if (irq == IRQ_S_EXT) {
        ppc64_xics_ack();
        driver_irq_dispatch(UART0_IRQ);
    }
}

#endif /* CONFIG_PPC64LE */
