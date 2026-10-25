#ifdef CONFIG_BOARD_QEMU_VIRT_X86_64

#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/audio/pc_speaker.h"
#include "drivers/char/cmos_rtc.h"
#include "core/arch.h"
#include "core/bootargs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "firmware.h"
#include "drivers/bus/pci_hal.h"

/* Bootargs come from the QEMU -append command line via fw_cfg.  On
 * non-QEMU firmware (empty cmdline) fall back to the static user-network
 * layout so networking does not depend on DHCP, which only progresses
 * while guest socket syscalls drive a20_lwip_poll(). */
const char *arch_bootargs_get(void) {
    const char *cmdline = firmware_bootargs();
    if (cmdline[0])
        return cmdline;
    return "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
           "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
           "a20.hostname=a20os-x86";
}

static void x86_64_irqchip_init(void) {}

static void x86_64_irqchip_enable(uint32_t irq) {
    /* Routed PCI vectors arrive masked; request_irq()'s auto-enable is the
     * only unmask path, which keeps source-less lines silent.  PIC-side
     * lines (keyboard/uart/mouse) are unmasked statically in pic_init. */
    x86_64_pci_irq_set_masked((int)irq, 0);
}

static void x86_64_irqchip_disable(uint32_t irq) {
    x86_64_pci_irq_set_masked((int)irq, 1);
}

static uint32_t x86_64_irqchip_ack(void) {
    return 0;
}

static void x86_64_irqchip_eoi(uint32_t irq) {
    (void)irq;
    lapic_write(LAPIC_EOI, 0);
}

static const irqchip_ops_t x86_64_irqchip_ops = {
    .init       = x86_64_irqchip_init,
    .enable_irq = x86_64_irqchip_enable,
    .disable_irq = x86_64_irqchip_disable,
    .ack        = x86_64_irqchip_ack,
    .eoi        = x86_64_irqchip_eoi,
};

static uint64_t x86_64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t x86_64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t x86_64_timer_ops = {
    .read_ticks    = x86_64_timer_read_ticks,
    .ticks_per_sec = x86_64_timer_ticks_per_sec,
};

static unsigned x86_64_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                     uint64_t boot_hw_id) {
#ifdef CONFIG_QEMU_SHARED_SMP
    (void)boot_hw_id;
    if (capacity < 2)
        return 0;
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        cpus[cpu].hw_id = cpu; /* Shared-memory endpoint, not remote APIC ID. */
        cpus[cpu].platform_cookie = cpu;
    }
    return 2;
#else
    uint32_t apic_ids[CONFIG_NR_CPUS];
    size_t count = firmware_acpi_apic_ids(apic_ids, capacity,
                                          (uint32_t)boot_hw_id);
    if (!count) {
        printf("[SMP] No valid ACPI MADT; using BSP only\n");
        cpus[0].hw_id = boot_hw_id;
        cpus[0].platform_cookie = 0;
        return 1;
    }
    for (size_t cpu = 0; cpu < count; cpu++) {
        cpus[cpu].hw_id = apic_ids[cpu];
        cpus[cpu].platform_cookie = apic_ids[cpu];
    }
    return (unsigned)count;
#endif
}

static int x86_64_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                            uintptr_t logical_context) {
    return x86_64_smp_start_ap((unsigned)cpu->hw_id, entry_pa,
                               (unsigned)logical_context);
}

static void x86_64_smp_send(const smp_cpu_desc_t *cpu,
                            smp_ipi_reason_t reason) {
    if (reason == SMP_IPI_RESCHEDULE)
        (void)x86_64_smp_send_ipi((unsigned)cpu->hw_id, IRQ_VECTOR_RESCHEDULE);
}

static void x86_64_smp_secondary(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    x86_64_smp_secondary_init();
}

static const smp_platform_ops_t x86_64_smp_ops = {
    .discover = x86_64_smp_discover,
    .start = x86_64_smp_start,
    .send_ipi = x86_64_smp_send,
#if CONFIG_NR_CPUS > 1
    .remote_tlb_flush = x86_64_smp_remote_tlb_flush,
#endif
    .secondary_init = x86_64_smp_secondary,
};

static void x86_64_early_init(void) {
    x86_64_enable_fpu_sse();
}

static void x86_64_poweroff(void) {
    outw(0x604, 0x2000);
    arch_halt();
}

static void x86_64_reboot(void) {
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02);
    outb(0x64, 0xFE);
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void x86_64_enumerate_buses(void) {
    /* MCFG bus numbers are inclusive; the PCI scanner takes an exclusive end.
     * Without MCFG, CF8/CFC is the PC-compatible config-space mechanism. */
    uintptr_t ecam = firmware_acpi_mcfg_base();
    int bus_start = 0, bus_end = 256;
    uint8_t first, last;
    if (ecam && firmware_acpi_mcfg_bus_range(&first, &last) == 0) {
        bus_start = first;
        bus_end = (int)last + 1;
    }
    printf("[PCI] %s configuration, buses %d..%d\n",
           ecam ? "MCFG ECAM" : "legacy CF8/CFC", bus_start, bus_end - 1);
    pci_enumerate(ecam, bus_start, bus_end);
}


void board_configure_devices(void);
#include "../../platform/legacy_devices.h"
static void x86_64_configure_devices(void) {
    board_configure_devices();
    x86_legacy_configure_devices();
}

static const board_config_t qemu_virt_x86_64 = {
    .configure_devices = x86_64_configure_devices,
    .name              = "qemu-virt-x86_64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &x86_64_irqchip_ops,
    .timer             = &x86_64_timer_ops,
    .smp               = &x86_64_smp_ops,
    .early_init        = x86_64_early_init,
    .poweroff          = x86_64_poweroff,
    .reboot            = x86_64_reboot,
    .enumerate_buses = x86_64_enumerate_buses,
};

const board_config_t *const current_board = &qemu_virt_x86_64;

#endif
