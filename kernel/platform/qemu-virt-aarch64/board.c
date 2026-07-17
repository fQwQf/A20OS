#ifdef CONFIG_AARCH64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

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
    (void)irq;
}

static uint32_t aa64_gic_ack(void) {
    /* The exception entry already claimed the interrupt through GICC_IAR. */
    return 0;
}

static void aa64_gic_eoi(uint32_t irq) {
    *aa64_gicc_reg32(0x0010) = irq;
}

static void aa64_gic_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t aa64_gic_ops = {
    .init       = aa64_gic_init,
    .enable_irq = aa64_gic_enable,
    .disable_irq = aa64_gic_disable,
    .ack        = aa64_gic_ack,
    .eoi        = aa64_gic_eoi,
    .send_ipi   = aa64_gic_send_ipi,
};

static uint64_t aa64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t aa64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t aa64_generic_timer_ops = {
    .read_ticks    = aa64_timer_read_ticks,
    .ticks_per_sec = aa64_timer_ticks_per_sec,
};

static void aa64_early_init(void) {
}

/* This board does not yet import QEMU's FDT /chosen/bootargs.  Keep the
 * userspace network regression path deterministic until that handoff exists. */
const char *arch_bootargs_get(void) {
    return "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
           "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
           "a20.hostname=a20os-qemu";
}

static void aa64_poweroff(void) {
    sbi_shutdown();
}

static void aa64_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void aa64_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 16);
}

static const board_config_t qemu_virt_aa64 = {
    .name              = "qemu-virt-aa64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &aa64_gic_ops,
    .timer             = &aa64_generic_timer_ops,
    .early_init        = aa64_early_init,
    .poweroff          = aa64_poweroff,
    .reboot            = aa64_reboot,
    .enumerate_devices = aa64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_aa64;

#endif /* CONFIG_AARCH64 */
