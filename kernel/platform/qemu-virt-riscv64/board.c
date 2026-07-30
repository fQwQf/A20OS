#ifdef CONFIG_RISCV64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/timer.h"
#include "firmware.h"

static void rv64_plic_init(void) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) = 0;
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static void rv64_plic_enable(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
    *(volatile uint32_t *)PLIC_SENABLE(hart) |= (1U << irq);
    *(volatile uint32_t *)(PLIC_PRIORITY + (uint64_t)irq * 4) = 1;
}

static void rv64_plic_disable(uint32_t irq) {
    int hart = (int)arch_cpu_hart_id(cpu_current_id());
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

static const irqchip_ops_t rv64_plic_ops = {
    .init       = rv64_plic_init,
    .enable_irq = rv64_plic_enable,
    .disable_irq = rv64_plic_disable,
    .ack        = rv64_plic_ack,
    .eoi        = rv64_plic_eoi,
};

static uint64_t rv64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t rv64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rv64_sbi_timer_ops = {
    .read_ticks    = rv64_timer_read_ticks,
    .ticks_per_sec = rv64_timer_ticks_per_sec,
};

static unsigned rv64_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                  uint64_t boot_hart) {
    for (unsigned cpu = 0; cpu < capacity; cpu++) {
        cpus[cpu].hw_id = (boot_hart + cpu) % capacity;
        cpus[cpu].platform_cookie = 0;
    }
    return capacity;
}

static int rv64_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                          uintptr_t logical_context) {
    return (int)sbi_hart_start(cpu->hw_id, entry_pa, logical_context);
}

static void rv64_smp_send_ipi(const smp_cpu_desc_t *cpu,
                              smp_ipi_reason_t reason) {
    (void)reason;
    sbi_send_ipi(1UL, cpu->hw_id);
}

static int rv64_smp_remote_tlb_flush(uint32_t pending, uint64_t addr,
                                     uint64_t size) {
    while (pending) {
        uint64_t base = ~(uint64_t)0;
        for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
            if (pending & (1U << cpu)) {
                uint64_t hw_id;
                if (!smp_logical_to_hw(cpu, &hw_id) && hw_id < base)
                    base = hw_id;
            }
        }
        if (base == ~(uint64_t)0)
            return -1;

        uint64_t hart_mask = 0;
        uint32_t sent = 0;
        for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
            uint64_t hw_id;
            if (!(pending & (1U << cpu)) ||
                smp_logical_to_hw(cpu, &hw_id) < 0 ||
                hw_id < base || hw_id - base >= 64)
                continue;
            hart_mask |= 1ULL << (hw_id - base);
            sent |= 1U << cpu;
        }
        if (!hart_mask ||
            sbi_remote_sfence_vma(hart_mask, base, addr, size) < 0)
            return -1;
        pending &= ~sent;
    }
    return 0;
}

static void rv64_smp_secondary_init(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    rv64_plic_init();
}

static const smp_platform_ops_t rv64_smp_ops = {
    .discover       = rv64_smp_discover,
    .start          = rv64_smp_start,
    .send_ipi       = rv64_smp_send_ipi,
    .remote_tlb_flush = rv64_smp_remote_tlb_flush,
    .secondary_init = rv64_smp_secondary_init,
};

static void rv64_early_init(void) {
    riscv64_memory_init();
}

static void rv64_poweroff(void) {
    sbi_shutdown();
}

static void rv64_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);
extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void rv64_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 1);
    pci_enumerate(PCIE_ECAM_BASE, 0, 1);
}

static const board_config_t qemu_virt_rv64 = {
    .name              = "qemu-virt-rv64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &rv64_plic_ops,
    .timer             = &rv64_sbi_timer_ops,
    .smp               = &rv64_smp_ops,
    .early_init        = rv64_early_init,
    .poweroff          = rv64_poweroff,
    .reboot            = rv64_reboot,
    .enumerate_devices = rv64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv64;

#endif /* CONFIG_RISCV64 */
