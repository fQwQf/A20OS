#ifdef CONFIG_BOARD_QEMU_VIRT_ARM32

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/smp.h"
#include "core/bootargs.h"

/* A strong board hook must override core/bootargs.c's empty weak default.
 * Physical boards may instead parse and reserve their own firmware DTB. */
extern const char *arm32_fdt_bootargs_get(void);
const char *arch_bootargs_get(void) {
    return arm32_fdt_bootargs_get();
}

#if CONFIG_NR_CPUS > 8
#error "ARM32 QEMU virt SMP uses GICv2 and supports at most 8 CPUs"
#endif

static inline volatile uint32_t *arm32_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint8_t *arm32_gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

static void arm32_gicd_set_target(uint32_t irq, uint8_t target) {
    uint32_t off = 0x800 + (irq & ~3U);
    uint32_t shift = (irq & 3U) * 8U;
    volatile uint32_t *reg = arm32_gicd_reg32(off);
    uint32_t value = *reg;
    value = (value & ~(0xFFU << shift)) | ((uint32_t)target << shift);
    *reg = value;
}

static void arm32_irqchip_init(void) {
}

static void arm32_irqchip_enable(uint32_t irq) {
    *arm32_gicd_reg32(0x100 + (irq / 32U) * 4U) = 1U << (irq % 32U);
    *arm32_gicd_reg8(0x400 + irq) = 0x40;
    /*
     * QEMU's ARM virt GIC accepts ITARGETSR updates as word accesses.  A byte
     * store leaves the SPI pending but unroutable, so input events never reach
     * the CPU even though the VirtIO device and queue are otherwise healthy.
     */
    arm32_gicd_set_target(irq, 0x01);
    arch_mb();
}

static void arm32_irqchip_disable(uint32_t irq) {
    *arm32_gicd_reg32(0x180 + (irq / 32U) * 4U) = 1U << (irq % 32U);
    arch_mb();
}

static uint32_t arm32_irqchip_ack(void) {
    /* The ARM exception entry already claims the interrupt from the GIC. */
    return 0;
}

static void arm32_irqchip_eoi(uint32_t irq) {
    /* arch_handle_irq() completes the claimed interrupt after dispatch. */
    (void)irq;
}

static const irqchip_ops_t arm32_irqchip_ops = {
    .init = arm32_irqchip_init,
    .enable_irq = arm32_irqchip_enable,
    .disable_irq = arm32_irqchip_disable,
    .ack = arm32_irqchip_ack,
    .eoi = arm32_irqchip_eoi,
};

static uint64_t arm32_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t arm32_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t arm32_timer_ops = {
    .read_ticks = arm32_timer_read_ticks,
    .ticks_per_sec = arm32_timer_ticks_per_sec,
};

static void arm32_early_init(void) {
}

static void arm32_poweroff(void) {
    firmware_shutdown();
}

static void arm32_reboot(void) {
    firmware_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void arm32_enumerate_devices(void) {
    virtio_mmio_enumerate((VIRTIO_BASE - PAGE_OFFSET), 8, 48);
}

static unsigned arm32_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity, uint64_t boot) {
    if (boot) return 0;
    for (unsigned i = 0; i < capacity; i++) {
        cpus[i].hw_id = i;
        cpus[i].platform_cookie = 0;
    }
    return capacity;
}
static int arm32_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry, uintptr_t context) {
    return firmware_cpu_on(cpu->hw_id, entry, context);
}
static void arm32_smp_ipi(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason) {
    (void)reason;
    arch_wmb();
    *arm32_gicd_reg32(0xf00) = (1u << (16 + cpu->hw_id)) | IRQ_S_SOFT;
}
static void arm32_smp_secondary_init(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    volatile uint32_t *gicc = (void *)(uintptr_t)GICC_BASE;
    gicc[0] = 0; gicc[1] = 0xff; gicc[0] = 1;
}
static const smp_platform_ops_t arm32_smp_ops = {
    .discover = arm32_smp_discover, .start = arm32_smp_start,
    .send_ipi = arm32_smp_ipi, .secondary_init = arm32_smp_secondary_init,
};

static const board_config_t qemu_virt_arm32 = {
    .name = "qemu-virt-arm32",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &arm32_irqchip_ops,
    .timer = &arm32_timer_ops,
    .smp = &arm32_smp_ops,
    .early_init = arm32_early_init,
    .poweroff = arm32_poweroff,
    .reboot = arm32_reboot,
    .enumerate_devices = arm32_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_arm32;

#endif
