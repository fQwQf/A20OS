#ifdef CONFIG_LOONGARCH32

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"

extern void trap_entry_la32(void);

static void handle_timer_irq(int from_user) {
    /* TICLR (CSR 0x44) bit 0 clears the timer interrupt flag. */
    __asm__ __volatile__("csrwr %0, 0x44" :: "r"(1UL) : "memory");
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
    proc_sched_tick(from_user);
}

void trap_init(void) {
    arch_write_tvec((uint32_t)trap_entry_la32);

    /*
     * ECFG (CSR 0x4): unmask the timer (IS[11]) and the first external
     * interrupt line (IS[2]) used by the NaiLoong SoC UART / devices.
     */
    uint32_t ecfg;
    __asm__ __volatile__("csrrd %0, 0x4" : "=r"(ecfg));
    ecfg |= (1UL << 11) | (1UL << 2);
    __asm__ __volatile__("csrwr %0, 0x4" :: "r"(ecfg));
}

void arch_handle_irq(uint32_t irq, int from_user) {
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == IRQ_S_EXT) {
        driver_irq_dispatch((uint32_t)UART0_IRQ);
    }
}

#endif
