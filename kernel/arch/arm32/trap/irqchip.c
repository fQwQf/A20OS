#ifdef CONFIG_ARM32

#include "core/trap.h"
#include "proc/proc.h"
#include "core/timer.h"
#include "drivers/core/driver_hwapi.h"
#include "core/progress.h"

volatile uint32_t arm32_trap_flags;
volatile uint32_t arm32_fault_addr;
volatile uint32_t arm32_fault_pc;

static inline volatile uint32_t *gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint32_t *gicc_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static inline volatile uint8_t *gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

#define GICD_CTLR          0x000
#define GICD_ISENABLER(n)  (0x100 + (uint32_t)(n) * 4)
#define GICD_IPRIORITYR(n) (0x400 + (uint32_t)(n))
#define GICD_ITARGETSR(n)  (0x800 + (uint32_t)(n))
#define GICC_CTLR          0x0000
#define GICC_PMR           0x0004
#define GICC_IAR           0x000C
#define GICC_EOIR          0x0010

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

uint32_t arm32_gic_ack(void) {
    return *gicc_reg32(GICC_IAR) & 0x3FFU;
}

static void gic_init(void) {
    *gicd_reg32(GICD_CTLR) = 0;
    *gicc_reg32(GICC_CTLR) = 0;
    gic_set_priority(IRQ_S_TIMER, 0x40);
    gic_set_priority(UART0_IRQ, 0x40);
    gic_set_target(IRQ_S_TIMER, 0x01);
    gic_set_target(UART0_IRQ, 0x01);
    gic_enable_irq(IRQ_S_TIMER);
    gic_enable_irq(UART0_IRQ);
    *gicc_reg32(GICC_PMR) = 0xFF;
    *gicc_reg32(GICC_CTLR) = 1;
    *gicd_reg32(GICD_CTLR) = 1;
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
#ifndef CONFIG_ARM32
    proc_yield();
#endif
}

void trap_init(void) {
    arch_write_tvec((uint64_t)arm32_vector_table);
    gic_init();
}

void arch_handle_irq(reg_t irq, int from_user) {
    if (irq >= 1020)
        return;
    if (irq == IRQ_S_TIMER) {
        handle_timer_irq(from_user);
        return;
    }
    driver_irq_dispatch((uint32_t)irq);
    gic_eoi((uint32_t)irq);
}

#endif
