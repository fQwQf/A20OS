
#include "drivers/core/driver_core.h"
#include "drivers/irqchip/plic.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/smp.h"
#include "firmware.h"

/* Shared PLIC body: see kernel/drivers/irqchip/plic.c.  riscv32 has no
 * arch_cpu_hart_id(), so the hart id is the logical cpu id. */
static uint64_t rv32_plic_hart_id(void) {
    const smp_cpu_desc_t *cpu = smp_cpu_desc(cpu_current_id());
    return cpu ? cpu->hw_id : arch_smp_boot_hw_id();
}

static uint64_t rv32_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t rv32_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rv32_timer_ops = {
    .read_ticks = rv32_timer_read_ticks,
    .ticks_per_sec = rv32_timer_ticks_per_sec,
};

static unsigned rv32_smp_discover(smp_cpu_desc_t *cpus, unsigned n, uint64_t boot)
{
    for (unsigned i = 0; i < n; i++) {
        cpus[i].hw_id = (boot + i) % n;
        cpus[i].platform_cookie = 0;
    }
    return n;
}
static int rv32_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry, uintptr_t context)
{
    return (int32_t)sbi_call(0x48534d, 0, cpu->hw_id, entry, context);
}
static void rv32_smp_ipi(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason)
{
    (void)reason;
    sbi_call(0x735049, 0, 1, cpu->hw_id, 0);
}
static int rv32_smp_rfence(uint32_t cpus, uint64_t addr, uint64_t size)
{
    (void)addr; (void)size;
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++) {
        if (!(cpus & (1U << i))) continue;
        const smp_cpu_desc_t *cpu = smp_cpu_desc(i);
        register uint32_t a0 __asm__("a0") = 1;
        register uint32_t a1 __asm__("a1") = cpu->hw_id;
        register uint32_t a2 __asm__("a2") = 0;
        register uint32_t a3 __asm__("a3") = (uint32_t)-1;
        register uint32_t a6 __asm__("a6") = 1;
        register uint32_t a7 __asm__("a7") = 0x52464e43;
        __asm__ volatile("ecall" : "+r"(a0), "+r"(a1)
                         : "r"(a2), "r"(a3), "r"(a6), "r"(a7) : "memory");
        if (a0) return -1;
    }
    return 0;
}
static const smp_platform_ops_t rv32_smp_ops = {
    .discover = rv32_smp_discover, .start = rv32_smp_start,
    .send_ipi = rv32_smp_ipi, .remote_tlb_flush = rv32_smp_rfence,
};

static void rv32_early_init(void) {
    plic_configure(PLIC_BASE, rv32_plic_hart_id);
}

static void rv32_poweroff(void) {
    sbi_shutdown();
}

static void rv32_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void rv32_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_PHYS_BASE, 8, 1);
}

static const board_config_t qemu_virt_rv32 = {
    .name = "qemu-virt-rv32",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &plic_irqchip_ops,
    .timer = &rv32_timer_ops,
    .smp = &rv32_smp_ops,
    .early_init = rv32_early_init,
    .poweroff = rv32_poweroff,
    .reboot = rv32_reboot,
    .enumerate_devices = rv32_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv32;
