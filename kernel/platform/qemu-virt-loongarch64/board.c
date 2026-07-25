#ifdef CONFIG_LOONGARCH64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/smp.h"
#include "core/timer.h"

#define IPI_BOOT_VECTOR       0
#define IPI_RESCHEDULE_VECTOR 1
#define IOCSR_MBUF_SEND       0x1048
#define IOCSR_SEND_BLOCKING   (1UL << 31)
#define IOCSR_SEND_CPU_SHIFT  16
#define IOCSR_MBUF_DATA_SHIFT 32

static void la64_irqchip_init(void) {
    arch_irqchip_init();
}

static void la64_irqchip_enable(uint32_t irq) {
    (void)irq;
    arch_irqchip_enable();
}

static void la64_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t la64_irqchip_ack(void) {
    return 0;
}

static void la64_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t la64_irqchip_ops = {
    .init       = la64_irqchip_init,
    .enable_irq = la64_irqchip_enable,
    .disable_irq = la64_irqchip_disable,
    .ack        = la64_irqchip_ack,
    .eoi        = la64_irqchip_eoi,
};

static uint64_t la64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t la64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t la64_timer_ops = {
    .read_ticks    = la64_timer_read_ticks,
    .ticks_per_sec = la64_timer_ticks_per_sec,
};

static unsigned la64_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                  uint64_t boot_hw_id) {
    unsigned count = capacity;
    for (unsigned cpu = 0; cpu < count; cpu++) {
        cpus[cpu].hw_id = cpu;
        cpus[cpu].platform_cookie = cpu;
    }
    return boot_hw_id < count ? count : 0;
}

static int la64_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                          uintptr_t logical_context) {
    (void)logical_context;
    uint64_t common = IOCSR_SEND_BLOCKING |
                      (cpu->hw_id << IOCSR_SEND_CPU_SHIFT);

    /* QEMU consumes mailbox 0 as two masked 32-bit writes. */
    loongarch64_iocsr_write64(
        common | (1UL << 2) | (entry_pa & 0xffffffff00000000UL),
        IOCSR_MBUF_SEND);
    loongarch64_iocsr_write64(
        common | ((uint64_t)(uint32_t)entry_pa << IOCSR_MBUF_DATA_SHIFT),
        IOCSR_MBUF_SEND);
    loongarch64_smp_send_ipi((unsigned)cpu->hw_id, IPI_BOOT_VECTOR);
    return 0;
}

static void la64_smp_send(const smp_cpu_desc_t *cpu,
                          smp_ipi_reason_t reason) {
    if (reason == SMP_IPI_RESCHEDULE)
        loongarch64_smp_send_ipi((unsigned)cpu->hw_id,
                                 IPI_RESCHEDULE_VECTOR);
}

static void la64_smp_secondary(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    loongarch64_smp_local_init();
}

static const smp_platform_ops_t la64_smp_ops = {
    .discover = la64_smp_discover,
    .start = la64_smp_start,
    .send_ipi = la64_smp_send,
    .secondary_init = la64_smp_secondary,
};

static void la64_early_init(void) {
    loongarch64_smp_local_init();
}

static void la64_poweroff(void) {
    *(volatile uint16_t *)(uintptr_t)VIRT_GED_SLEEP_CTL =
        (uint16_t)((VIRT_GED_SLP_TYP_S5 << VIRT_GED_SLP_TYP_POS) | VIRT_GED_SLP_EN);
    arch_halt();
}

static void la64_reboot(void) {
    *(volatile uint16_t *)(uintptr_t)VIRT_GED_RESET_REG = VIRT_GED_RESET_VAL;
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void la64_enumerate_devices(void) {
    pci_enumerate(PCIE_ECAM_BASE, PCIE_BUS_START, PCIE_BUS_END);
}

static const board_config_t qemu_virt_la64 = {
    .name              = "qemu-virt-la64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &la64_irqchip_ops,
    .timer             = &la64_timer_ops,
    .smp               = &la64_smp_ops,
    .early_init        = la64_early_init,
    .poweroff          = la64_poweroff,
    .reboot            = la64_reboot,
    .enumerate_devices = la64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_la64;

#endif /* CONFIG_LOONGARCH64 */
