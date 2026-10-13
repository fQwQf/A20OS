#include "gicv2_device.h"
#include "core/arch.h"

#ifndef CONFIG_AARCH64_GICV3
static inline volatile uint32_t *aa64_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint32_t *aa64_gicc_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static inline volatile uint8_t *aa64_gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

static void aa64_gicd_set_target(uint32_t irq, uint8_t target) {
    uint32_t off = 0x800 + (irq & ~3U);
    uint32_t shift = (irq & 3U) * 8U;
    volatile uint32_t *reg = aa64_gicd_reg32(off);
    uint32_t value = *reg;

    value = (value & ~(0xFFU << shift)) | ((uint32_t)target << shift);
    *reg = value;
}

static void aa64_gic_init(void) {
    *aa64_gicd_reg32(0x000) = 0;
    *aa64_gicc_reg32(0x0000) = 0;

    *aa64_gicc_reg32(0x0004) = 0xFF;
    *aa64_gicc_reg32(0x0000) = 1;
    *aa64_gicd_reg32(0x000) = 1;
}

static void aa64_gic_enable(uint32_t irq) {
    *aa64_gicd_reg32(0x100 + (uint32_t)(irq / 32U) * 4) = 1U << (irq % 32U);
    *aa64_gicd_reg8(0x400 + (uint32_t)irq) = 0x40;
    /* QEMU virt requires a word write for GICD_ITARGETSR updates. */
    aa64_gicd_set_target(irq, 0x01);
    arch_mb();
}

static void aa64_gic_disable(uint32_t irq) {
    *aa64_gicd_reg32(0x180 + (irq / 32U) * 4) = 1U << (irq % 32U);
    arch_mb();
}

static uint32_t aa64_gic_ack(void) {
    /* The exception entry already claimed the interrupt through GICC_IAR. */
    return 0;
}

static void aa64_gic_eoi(uint32_t irq) {
    *aa64_gicc_reg32(0x0010) = irq;
}

const irqchip_ops_t aarch64_gicv2_ops = {
    .init = aa64_gic_init,
    .enable_irq = aa64_gic_enable,
    .disable_irq = aa64_gic_disable,
    .ack = aa64_gic_ack,
    .eoi = aa64_gic_eoi,
};

#endif
