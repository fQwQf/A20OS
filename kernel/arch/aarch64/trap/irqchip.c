#ifdef CONFIG_AARCH64

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drv/uart.h"

volatile uint64_t aarch64_trap_flags;

static inline volatile uint32_t *gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint32_t *gicc_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static inline volatile uint8_t *gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

#define GICD_CTLR           0x000
#define GICD_ISENABLER(n)   (0x100 + (uint32_t)(n) * 4)
#define GICD_IPRIORITYR(n)  (0x400 + (uint32_t)(n))
#define GICD_ITARGETSR(n)   (0x800 + (uint32_t)(n))

#define GICC_CTLR           0x0000
#define GICC_PMR            0x0004
#define GICC_IAR            0x000C
#define GICC_EOIR           0x0010

#define ESR_EC_UNKNOWN      0x00U
#define ESR_EC_SVC64        0x15U
#define ESR_EC_IABT_LOW     0x20U
#define ESR_EC_IABT_CUR     0x21U
#define ESR_EC_PC_ALIGN     0x22U
#define ESR_EC_DABT_LOW     0x24U
#define ESR_EC_DABT_CUR     0x25U
#define ESR_EC_SP_ALIGN     0x26U
#define ESR_EC_BRK_LOW      0x3CU
#define ESR_ISS_WNR         (1U << 6)

static inline int aarch64_abort_is_page_fault(uint64_t esr) {
    uint64_t fsc = esr & 0x3FUL;
    switch (fsc) {
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x09: case 0x0A: case 0x0B:
    case 0x0D: case 0x0E: case 0x0F:
        return 1;
    default:
        return 0;
    }
}

uint64_t aarch64_decode_sync_cause(uint64_t esr) {
    uint64_t ec = (esr >> 26) & 0x3FUL;
    switch (ec) {
    case ESR_EC_SVC64:
        return CAUSE_ECALL_U;
    case ESR_EC_IABT_LOW:
    case ESR_EC_IABT_CUR:
        return aarch64_abort_is_page_fault(esr) ? CAUSE_INSN_PAGE_FAULT : CAUSE_INSN_FAULT;
    case ESR_EC_DABT_LOW:
    case ESR_EC_DABT_CUR:
        if (aarch64_abort_is_page_fault(esr))
            return (esr & ESR_ISS_WNR) ? CAUSE_STORE_PAGE_FAULT : CAUSE_LOAD_PAGE_FAULT;
        return (esr & ESR_ISS_WNR) ? CAUSE_STORE_FAULT : CAUSE_LOAD_FAULT;
    case ESR_EC_PC_ALIGN:
        return CAUSE_INSN_FAULT;
    case ESR_EC_SP_ALIGN:
        return CAUSE_STORE_FAULT;
    case ESR_EC_BRK_LOW:
        return CAUSE_BREAKPOINT;
    case ESR_EC_UNKNOWN:
    default:
        return CAUSE_ILLEGAL_INSN;
    }
}

static void gic_enable_irq(uint32_t irq) {
    *gicd_reg32(GICD_ISENABLER(irq / 32U)) = 1U << (irq % 32U);
}

static void gic_set_priority(uint32_t irq, uint8_t prio) {
    *gicd_reg8(GICD_IPRIORITYR(irq)) = prio;
}

static void gic_set_target(uint32_t irq, uint8_t mask) {
    *gicd_reg8(GICD_ITARGETSR(irq)) = mask;
}

static void gic_eoi(uint32_t irq) {
    *gicc_reg32(GICC_EOIR) = irq;
}

uint64_t aarch64_gic_ack(void) {
    return *gicc_reg32(GICC_IAR) & 0x3FFU;
}

static void gic_init(void) {
    *gicd_reg32(GICD_CTLR) = 0;
    *gicc_reg32(GICC_CTLR) = 0;

    gic_set_priority(IRQ_S_TIMER, 0x40);
    gic_set_priority(UART0_IRQ, 0x40);
    gic_set_target(UART0_IRQ, 0x01);
    gic_enable_irq(IRQ_S_TIMER);
    gic_enable_irq(UART0_IRQ);

    *gicc_reg32(GICC_PMR) = 0xFF;
    *gicc_reg32(GICC_CTLR) = 1;
    *gicd_reg32(GICD_CTLR) = 1;
}

static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    timer_set_interval(TICKS_PER_SEC / 100);
    gic_eoi(IRQ_S_TIMER);
    if (!from_user)
        return;

    task_t *cur = proc_current();
    if (cur)
        cur->total_time++;
    proc_yield();
}

void trap_init(void) {
    arch_write_tvec((uint64_t)aarch64_vector_table);
    gic_init();
}

void arch_handle_irq(uint64_t irq, int from_user) {
    if (irq >= 1020)
        return;

    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }

    if (irq == UART0_IRQ) {
        uart_handle_irq();
        gic_eoi((uint32_t)irq);
        return;
    }

    gic_eoi((uint32_t)irq);
}

#endif /* CONFIG_AARCH64 */
