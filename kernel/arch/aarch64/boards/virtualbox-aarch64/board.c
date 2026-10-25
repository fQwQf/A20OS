
#include "drivers/core/driver_core.h"
#include "drivers/bus/pci_bus.h"
#include "core/arch.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "firmware/acpi.h"
#include "boot/context.h"

extern uint64_t aarch64_boot_acpi_rsdp;

static inline volatile uint32_t *vbox_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static void vbox_gic_init(void) {
    /* The architecture trap layer owns GICv3 distributor/CPU init. */
}

static void vbox_gic_enable(uint32_t irq) {
    volatile uint32_t *base =
        irq < 32U ? (volatile uint32_t *)(uintptr_t)(GICR_BASE + 0x10000)
                  : vbox_gicd_reg32(0);
    base[(0x100 + (uint32_t)(irq / 32U) * 4) / 4] = 1U << (irq % 32U);
}

static void vbox_gic_disable(uint32_t irq) {
    volatile uint32_t *base =
        irq < 32U ? (volatile uint32_t *)(uintptr_t)(GICR_BASE + 0x10000)
                  : vbox_gicd_reg32(0);
    base[(0x180 + (uint32_t)(irq / 32U) * 4) / 4] = 1U << (irq % 32U);
}

static uint32_t vbox_gic_ack(void) {
    return 0;
}

static void vbox_gic_eoi(uint32_t irq) {
    __asm__ __volatile__("msr icc_eoir1_el1, %0" ::"r"((uint64_t)irq)
                         : "memory");
}

static const irqchip_ops_t vbox_aa64_gic_ops = {
    .init = vbox_gic_init,
    .enable_irq = vbox_gic_enable,
    .disable_irq = vbox_gic_disable,
    .ack = vbox_gic_ack,
    .eoi = vbox_gic_eoi,
};

static uint64_t vbox_aa64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t vbox_aa64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t vbox_aa64_timer_ops = {
    .read_ticks = vbox_aa64_timer_read_ticks,
    .ticks_per_sec = vbox_aa64_timer_ticks_per_sec,
};

static void vbox_aa64_early_init(void) {
}

/* The UEFI handoff currently has no command-line channel.  Use VBox NAT's
 * documented guest subnet directly so networking does not depend on timer
 * driven DHCP retries while this board still uses its software timer. */
/*
 * The loader hands over whatever the firmware passed as LoadOptions, which is how
 * every other A20OS platform receives its command line and how a root= for a
 * second Alpine disk gets here at all.  Falling back to a compiled-in string
 * when the firmware supplied nothing keeps a bare `a20 run` working, which is
 * the common case; it is a default, not the mechanism.
 */
extern uint64_t aarch64_boot_cmdline;

const char *arch_bootargs_get(void) {
    static const char defaults[] = "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
                                   "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
                                   "a20.hostname=a20os-vbox";

    const char *cmdline = boot_context_get()->command_line;
    if (cmdline && cmdline[0])
        return cmdline;
    return defaults;
}

static void vbox_aa64_poweroff(void) {
    sbi_shutdown();
}

static void vbox_aa64_reboot(void) {
    sbi_reboot();
}

static void vbox_aa64_enumerate_devices(void) {
    printf("[VBOX] ACPI RSDP: 0x%lx\n", (unsigned long)aarch64_boot_acpi_rsdp);
    const acpi_context_t *context = arch_acpi_context();
    acpi_table_t mcfg;
    if (!context || acpi_table_find(context, "MCFG", 0, &mcfg)) {
        printf("[VBOX] ACPI MCFG unavailable (RSDP=0x%lx)\n",
               (unsigned long)aarch64_boot_acpi_rsdp);
        return;
    }
    size_t count;
    int enumerated = 0;
    if (!acpi_mcfg_count(&mcfg, &count)) {
        for (size_t i = 0; i < count; i++) {
            acpi_mcfg_entry_t e;
            if (acpi_mcfg_get(&mcfg, i, &e) || e.segment ||
                e.base > 0x100000000ULL - (((uint64_t)e.end_bus + 1) << 20))
                continue;
            uintptr_t ecam = (uintptr_t)(PAGE_OFFSET + e.base);
            printf("[VBOX] PCIe ECAM: 0x%lx, buses %u-%u\n",
                   (unsigned long)ecam, e.start_bus, e.end_bus);
            pci_enumerate(ecam, e.start_bus, (int)e.end_bus + 1);
            enumerated = 1;
            break;
        }
    }
    acpi_table_release(context, &mcfg);
    if (!enumerated)
        printf("[VBOX] no usable PCIe ECAM allocation in MCFG\n");
}

static const board_config_t virtualbox_aarch64 = {
    .name = "virtualbox-aarch64",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &vbox_aa64_gic_ops,
    .timer = &vbox_aa64_timer_ops,
    .early_init = vbox_aa64_early_init,
    .poweroff = vbox_aa64_poweroff,
    .reboot = vbox_aa64_reboot,
    .enumerate_buses = vbox_aa64_enumerate_devices,
};

const board_config_t *const current_board = &virtualbox_aarch64;
