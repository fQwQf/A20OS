#ifdef CONFIG_AARCH64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

static inline volatile uint32_t *vbox_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static void vbox_gic_init(void) {
    /* The architecture trap layer owns GICv3 distributor/CPU init. */
}

static void vbox_gic_enable(uint32_t irq) {
    volatile uint32_t *base = irq < 32U
        ? (volatile uint32_t *)(uintptr_t)(GICR_BASE + 0x10000)
        : vbox_gicd_reg32(0);
    base[(0x100 + (uint32_t)(irq / 32U) * 4) / 4] = 1U << (irq % 32U);
}

static void vbox_gic_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t vbox_gic_ack(void) {
    return 0;
}

static void vbox_gic_eoi(uint32_t irq) {
    __asm__ __volatile__("msr icc_eoir1_el1, %0" :: "r"((uint64_t)irq) : "memory");
}

static void vbox_gic_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t vbox_aa64_gic_ops = {
    .init       = vbox_gic_init,
    .enable_irq = vbox_gic_enable,
    .disable_irq = vbox_gic_disable,
    .ack        = vbox_gic_ack,
    .eoi        = vbox_gic_eoi,
    .send_ipi   = vbox_gic_send_ipi,
};

static uint64_t vbox_aa64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t vbox_aa64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t vbox_aa64_timer_ops = {
    .read_ticks    = vbox_aa64_timer_read_ticks,
    .ticks_per_sec = vbox_aa64_timer_ticks_per_sec,
};

static void vbox_aa64_early_init(void) {
}

static void vbox_aa64_poweroff(void) {
    sbi_shutdown();
}

static void vbox_aa64_reboot(void) {
    sbi_reboot();
}

static void vbox_aa64_enumerate_devices(void) {
    /* VirtualBox ARM64 does not use virtio-mmio.  PCI/PCIe enumeration and
     * any UEFI-provided device tables would be added here once supported. */
}

static const board_config_t virtualbox_aarch64 = {
    .name              = "virtualbox-aarch64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &vbox_aa64_gic_ops,
    .timer             = &vbox_aa64_timer_ops,
    .early_init        = vbox_aa64_early_init,
    .poweroff          = vbox_aa64_poweroff,
    .reboot            = vbox_aa64_reboot,
    .enumerate_devices = vbox_aa64_enumerate_devices,
};

const board_config_t *const current_board = &virtualbox_aarch64;

#endif /* CONFIG_AARCH64 */
