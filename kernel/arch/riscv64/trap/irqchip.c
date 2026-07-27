#ifdef CONFIG_RISCV64

#include "core/trap.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"

static void plic_init_hart(void) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1U << UART0_IRQ);
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static uint32_t plic_claim(void) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

static void plic_complete(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SCLAIM(hart) = irq;
}

static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    kernel_progress_timer_tick();
    uint64_t now = timer_get_ticks();
    timer_set_interval(proc_next_timer_interval(now));
    proc_sched_tick(from_user);
}

void trap_init(void) {
    arch_write_tvec((uint64_t)__trap_from_kernel);
    arch_write_sscratch(0);
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
    plic_init_hart();
    arch_write_sie(arch_read_sie() | SIE_SEIE | SIE_SSIE);
}

void arch_handle_irq(uint64_t irq, int from_user) {
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id != 0)
            driver_irq_dispatch(irq_id);
        plic_complete(irq_id);
        return;
    }

    if (irq == IRQ_S_SOFT) {
        arch_write_sip(arch_read_sip() & ~SIE_SSIE);
#ifdef CONFIG_SMP
        /* The persistent need_resched flag is consumed at a safe return point. */
        proc_sched_handle_reschedule_ipi();
#else
        timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
        proc_sched_request_current();
#endif
    }
}

#endif /* CONFIG_RISCV64 */
