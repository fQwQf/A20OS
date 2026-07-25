#ifdef CONFIG_AARCH64

#include "core/trap.h"
#include "platform.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/char/uart.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"
#include "core/stdio.h"
#include "core/cpu.h"
#include "core/smp.h"

volatile uint64_t aarch64_trap_flags[CONFIG_NR_CPUS];
#ifndef CONFIG_AARCH64_GICV3
static uint32_t aarch64_gic_iar[CONFIG_NR_CPUS];
#endif

#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
void aarch64_reschedule_ipi_received(void);
#endif

static inline volatile uint32_t *gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint32_t *gicc_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static inline volatile uint8_t *gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

#ifdef CONFIG_AARCH64_GICV3
static inline volatile uint32_t *gicr_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICR_BASE + off);
}
#endif

#define GICD_CTLR           0x000
#define GICD_ISENABLER(n)   (0x100 + (uint32_t)(n) * 4)
#define GICD_IPRIORITYR(n)  (0x400 + (uint32_t)(n))
#define GICD_ITARGETSR(n)   (0x800 + (uint32_t)(n))
#define GIC_RESCHEDULE_SGI  IRQ_S_SOFT

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
#define ESR_FSC_ALIGN       0x21U

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
        if ((esr & 0x3FUL) == ESR_FSC_ALIGN)
            return CAUSE_LOAD_MISALIGNED;
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
#ifdef CONFIG_AARCH64_GICV3
    volatile uint32_t *base = irq < 32U ? gicr_reg32(0x10000) : gicd_reg32(0);
    base[(GICD_ISENABLER(irq / 32U)) / 4] = 1U << (irq % 32U);
#else
    *gicd_reg32(GICD_ISENABLER(irq / 32U)) = 1U << (irq % 32U);
#endif
}

static void gic_set_priority(uint32_t irq, uint8_t prio) {
#ifdef CONFIG_AARCH64_GICV3
    volatile uint8_t *base = irq < 32U
        ? (volatile uint8_t *)(uintptr_t)(GICR_BASE + 0x10000)
        : gicd_reg8(0);
    base[GICD_IPRIORITYR(irq)] = prio;
#else
    *gicd_reg8(GICD_IPRIORITYR(irq)) = prio;
#endif
}

#ifndef CONFIG_AARCH64_GICV3
static void gic_set_target(uint32_t irq, uint8_t mask) {
    *gicd_reg8(GICD_ITARGETSR(irq)) = mask;
}
#endif

static void gic_eoi(uint32_t irq) {
#ifdef CONFIG_AARCH64_GICV3
    __asm__ __volatile__("msr icc_eoir1_el1, %0" :: "r"((uint64_t)irq) : "memory");
#else
    unsigned cpu = cpu_current_id();
    uint32_t iar = aarch64_gic_iar[cpu];
    *gicc_reg32(GICC_EOIR) = (iar & 0x3FFU) == irq ? iar : irq;
#endif
}

uint64_t aarch64_gic_ack(void) {
#ifdef CONFIG_AARCH64_GICV3
    uint64_t irq;
    __asm__ __volatile__("mrs %0, icc_iar1_el1" : "=r"(irq));
    return irq & 0xFFFFFFU;
#else
    unsigned cpu = cpu_current_id();
    uint32_t iar = *gicc_reg32(GICC_IAR);
    aarch64_gic_iar[cpu] = iar;
    return iar & 0x3FFU;
#endif
}

static void gic_init(void) {
#ifdef CONFIG_AARCH64_GICV3
    uint64_t value;

    /* Enable the system-register CPU interface and Group 1 interrupts. */
    __asm__ __volatile__("mrs %0, icc_sre_el1" : "=r"(value));
    value |= 1U;
    __asm__ __volatile__("msr icc_sre_el1, %0\n\tisb" :: "r"(value) : "memory");

    *gicd_reg32(GICD_CTLR) = (1U << 4) | (1U << 1);
    while (*gicd_reg32(GICD_CTLR) & (1U << 31))
        arch_cpu_relax();

    /* Wake this CPU's redistributor. */
    uint32_t waker = *gicr_reg32(0x14);
    *gicr_reg32(0x14) = waker & ~(1U << 1);
    while (*gicr_reg32(0x14) & (1U << 2))
        arch_cpu_relax();

    /* The UART SPI is handled as non-secure Group 1. */
#ifndef CONFIG_AARCH64_COOPERATIVE_BOOT
    /* The normal AArch64 boards use the architected timer PPI as Group 1. */
    *gicr_reg32(0x10000 + 0x80) = 1U << IRQ_S_TIMER;
#endif
    *gicd_reg32(0x80 + (UART0_IRQ / 32U) * 4) = 1U << (UART0_IRQ % 32U);
    *(volatile uint64_t *)(uintptr_t)(GICD_BASE + 0x6000 + UART0_IRQ * 8U) = 0;

#ifndef CONFIG_AARCH64_COOPERATIVE_BOOT
    gic_set_priority(IRQ_S_TIMER, 0x40);
#endif
    gic_set_priority(UART0_IRQ, 0x40);
#ifndef CONFIG_AARCH64_COOPERATIVE_BOOT
    gic_enable_irq(IRQ_S_TIMER);
#endif
    gic_enable_irq(UART0_IRQ);

    value = 0xFF;
    __asm__ __volatile__("msr icc_pmr_el1, %0" :: "r"(value) : "memory");
    value = 0;
    __asm__ __volatile__("msr icc_bpr1_el1, %0" :: "r"(value) : "memory");
    value = 1;
    __asm__ __volatile__("msr icc_igrpen1_el1, %0\n\tisb" :: "r"(value) : "memory");
#else
    unsigned cpu = cpu_current_id();
    if (cpu == 0)
        *gicd_reg32(GICD_CTLR) = 0;
    *gicc_reg32(GICC_CTLR) = 0;

    gic_set_priority(GIC_RESCHEDULE_SGI, 0x20);
    gic_set_priority(IRQ_S_TIMER, 0x40);
    if (cpu == 0) {
        gic_set_priority(UART0_IRQ, 0x40);
        gic_set_target(UART0_IRQ, 0x01);
    }
    gic_enable_irq(GIC_RESCHEDULE_SGI);
    gic_enable_irq(IRQ_S_TIMER);
    if (cpu == 0)
        gic_enable_irq(UART0_IRQ);

    *gicc_reg32(GICC_PMR) = 0xFF;
    *gicc_reg32(GICC_CTLR) = 1;
    if (cpu == 0)
        *gicd_reg32(GICD_CTLR) = 1;
#endif
}

static void handle_timer_irq(int from_user) {
    timer_irq_tick();
    kernel_progress_timer_tick();
    timer_set_interval(proc_next_timer_interval(timer_get_ticks()));
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

    if (irq == GIC_RESCHEDULE_SGI) {
        gic_eoi(GIC_RESCHEDULE_SGI);
#ifdef CONFIG_BOARD_QEMU_VIRT_AARCH64
        aarch64_reschedule_ipi_received();
#endif
        if (from_user)
            proc_yield();
        return;
    }

    driver_irq_dispatch((uint32_t)irq);
}

#endif /* CONFIG_AARCH64 */
