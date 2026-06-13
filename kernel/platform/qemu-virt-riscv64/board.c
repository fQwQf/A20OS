#ifdef CONFIG_RISCV64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"

static void rv64_plic_init(void) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) = 0;
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static void rv64_plic_enable(uint32_t irq) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) |= (1U << irq);
    *(volatile uint32_t *)(PLIC_PRIORITY + (uint64_t)irq * 4) = 1;
}

static void rv64_plic_disable(uint32_t irq) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) &= ~(1U << irq);
}

static uint32_t rv64_plic_ack(void) {
    /* PLIC claim is handled by arch_handle_irq(); this callback exists only
     * so driver_irq_dispatch() can optional-call ack without side effects. */
    return 0;
}

static void rv64_plic_eoi(uint32_t irq) {
    /* PLIC completion is handled by arch_handle_irq(); this callback exists
     * only so driver_irq_dispatch() can optional-call eoi without side effects. */
    (void)irq;
}

static void rv64_plic_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t rv64_plic_ops = {
    .init       = rv64_plic_init,
    .enable_irq = rv64_plic_enable,
    .disable_irq = rv64_plic_disable,
    .ack        = rv64_plic_ack,
    .eoi        = rv64_plic_eoi,
    .send_ipi   = rv64_plic_send_ipi,
};

static void rv64_timer_init(void) {
}

static void rv64_timer_set_interval(uint64_t ticks) {
    uint64_t now;
    __asm__ volatile("csrr %0, time" : "=r"(now));
    firmware_set_timer(now + ticks);
}

static uint64_t rv64_timer_read_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

static uint64_t rv64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rv64_sbi_timer_ops = {
    .init          = rv64_timer_init,
    .set_interval  = rv64_timer_set_interval,
    .read_ticks    = rv64_timer_read_ticks,
    .ticks_per_sec = rv64_timer_ticks_per_sec,
};

static void rv64_early_init(void) {
}

static void rv64_poweroff(void) {
    sbi_shutdown();
}

static void rv64_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void rv64_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 1);
}

static const board_config_t qemu_virt_rv64 = {
    .name              = "qemu-virt-rv64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &rv64_plic_ops,
    .timer             = &rv64_sbi_timer_ops,
    .early_init        = rv64_early_init,
    .poweroff          = rv64_poweroff,
    .reboot            = rv64_reboot,
    .enumerate_devices = rv64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv64;

#endif /* CONFIG_RISCV64 */
