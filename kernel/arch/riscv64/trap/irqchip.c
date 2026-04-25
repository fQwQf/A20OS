#ifdef CONFIG_RISCV64

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drv/uart.h"

static void plic_init_hart(void) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1U << UART0_IRQ);
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static uint32_t plic_claim(void) {
    int hart = 0;
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

static void plic_complete(uint32_t irq) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SCLAIM(hart) = irq;
}

static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    timer_set_interval(TICKS_PER_SEC / 100);
    if (!from_user) return;

    task_t *cur = proc_current();
    if (cur) cur->total_time++;
    proc_yield();
}

void trap_init(void) {
    arch_write_tvec((uint64_t)__trap_from_kernel);
    arch_write_sscratch(0);
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
    plic_init_hart();
    arch_write_sie(arch_read_sie() | SIE_SEIE);
}

void arch_handle_irq(uint64_t irq, int from_user) {
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id == UART0_IRQ)
            uart_handle_irq();
        if (irq_id != 0)
            plic_complete(irq_id);
        return;
    }

    if (irq == IRQ_S_SOFT) {
        arch_write_sip(arch_read_sip() & ~SIE_SSIE);
        timer_set_interval(TICKS_PER_SEC / 100);
        if (from_user)
            proc_yield();
    }
}

#endif /* CONFIG_RISCV64 */
