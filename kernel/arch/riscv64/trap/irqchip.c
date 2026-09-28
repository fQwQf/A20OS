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
    /* SENABLE is a per-context register (offset 0x2080 + hart * 0x100) and its
     * bit position is the PLIC interrupt ID itself, not an index into a smaller
     * slice.  Only UART0_IRQ is routed to this hart, so the word written here
     * is 2: every other bit is 0 and must stay 0, because a 1 would enable
     * whatever other source the board happens to wire to this context. */
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1U << UART0_IRQ);
    /* Priority threshold before the gateway above, so the two never disagree
     * about a source in the window between the stores. */
    /* SPRIORITY is the per-context preemption threshold: a pending source is
     * delivered when its global priority is >= this value.  Zero is the most
     * permissive threshold there is -- it admits every source that has a
     * priority at all -- and it still does not deliver a source whose
     * PLIC_PRIORITY is 0, because a zero priority is defined to mean "never
     * deliver" regardless of the threshold. */
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static uint32_t plic_claim(void) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    /* Reading the claim register is what picks the highest-priority pending
     * source for this context and simultaneously moves it to the in-service
     * state, so the ID returned here is the one the PLIC will not hand out
     * again until it is completed.  A read of 0 means "nothing pending": ID 0 is
     * reserved, can never be claimed, and must not be dispatched. */
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

static void plic_complete(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    /* The same register completes a claim: writing the ID back takes the source
     * out of the in-service state, which is the only thing that lets the PLIC
     * raise this context again for that source.  A forgotten completion is not a
     * lost interrupt but a permanently stuck one, so it is issued even when
     * irq is 0 -- writing 0 is a no-op, not an error. */
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
    /* The priority array starts at offset 0 with one 32-bit word per interrupt
     * ID, so UART0_IRQ's word is base + ID * 4.  PLIC_PRIORITY is global rather
     * than per-context, which is why re-running this from a secondary hart (see
     * smp_secondary_init) is harmless.  The value must be non-zero: a source
     * whose priority is 0 is never delivered no matter what this hart's
     * thresholds are, and 1 is the lowest priority that can still reach a
     * context programmed with SPRIORITY = 0. */
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
    /* plic_init_hart() derives the context from this CPU's own hart ID, so
     * calling trap_init() again from smp_secondary_init() programs that
     * secondary's own gateway and threshold: the PLIC selects the context by the
     * access address, with no hardware state to set up first. */
    plic_init_hart();
    /* sie bit 9 (SEIE) is the supervisor-wide gateway for the PLIC and bit 1
     * (SSIE) the software-interrupt gate, which is the only way the IPI branch
     * in arch_handle_irq() below can ever be entered.  OR-ed in rather than
     * assigned so the timer bit that earlier boot code may have set survives. */
    arch_write_sie(arch_read_sie() | SIE_SEIE | SIE_SSIE);
}

void arch_handle_irq(uint64_t irq, int from_user) {
    /* scause 5 is the supervisor timer interrupt raised straight from the
     * CLINT (ACLINT on the 32-bit port).  It is not a PLIC source, so it needs
     * neither a claim nor a completion. */
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    /* scause 9 is the supervisor external interrupt, the single S-level entry
     * point for the whole PLIC.  One claim per trap is deliberate: a second
     * pending source stays pending and re-enters here after this trap returns,
     * so there is nothing to drain in a loop, and looping instead would hold off
     * the timer for as long as the sources keep arriving. */
    if (irq == IRQ_S_EXT) {
        uint32_t irq_id = plic_claim();
        if (irq_id != 0)
            driver_irq_dispatch(irq_id);
        plic_complete(irq_id);
        return;
    }

    if (irq == IRQ_S_SOFT) {
        /* sip bit 1 (SSIP) is write-1-to-clear and this store touches only the
         * current hart's register, so the hart consumes its own pending software
         * interrupt and no other hart's IPI is lost. */
        arch_write_sip(arch_read_sip() & ~SIE_SSIE);
#ifdef CONFIG_SMP
        extern void rv64_ipi_tlb_flush_handler(void);
        rv64_ipi_tlb_flush_handler();
        /* The persistent need_resched flag is consumed at a safe return point. */
        proc_sched_handle_reschedule_ipi();
#else
        timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
        proc_sched_request_current();
#endif
    }
}

#ifdef CONFIG_SMP
/* Weak default so a RISC-V board that does not implement generation-based TLB
 * shootdowns (or does not enable SMP) still links; such a board must not set
 * smp_platform_ops.remote_tlb_flush unless its handler publishes acks. */
__attribute__((weak))
void rv64_ipi_tlb_flush_handler(void)
{
    __asm__ __volatile__("sfence.vma" ::: "memory");
}
#endif /* CONFIG_SMP */

#endif /* CONFIG_RISCV64 */
