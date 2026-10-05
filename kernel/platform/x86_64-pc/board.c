#ifdef CONFIG_BOARD_X86_64_PC

/*
 * Generic PC-compatible machine: refurbished thin clients, N100/J4125 fanless
 * mini PCs, and office boxes.  Nothing here is specific to one SKU; what the
 * board actually does is stop pretending the machine is QEMU.
 *
 * Three QEMU-only assumptions are replaced, in the order they bite:
 *
 *  1. The PCIe ECAM base was 0xB0000000, which is q35's MMCONFIG.  A 2011+
 *     chipset puts ECAM at 0xE0000000 or wherever MCFG says, so on real
 *     hardware every config read returned garbage and no PCI device was
 *     found.  early_init() now asks ACPI.
 *
 *  2. PCI INTx routing only recognised the q35 host bridge (0x29c08086), so
 *     every real chipset fell back to polling.  This board publishes the
 *     chipset's GSI base; see the caveat in x86_64-pc.md.
 *
 *  3. The kernel command line came from QEMU's fw_cfg device, which does not
 *     exist on a physical machine, so every a20.* knob was unreachable.  The
 *     multiboot info block is now read first (see firmware.c).
 *
 * Verified: builds clean under -Werror (check-x86_64-pc-build).  NOT run on
 * hardware.  See docs/platforms/x86_64-pc.md.
 *
 * RAM is not declared here: x86_64 discovers it from the multiboot memory map,
 * so the values below are only the window the boot page tables cover.
 */

#include "core/arch.h"
#include "core/bootargs.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/audio/pc_speaker.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "firmware.h"
#include "platform.h"

/* ACPI's default IOAPIC GSI numbering places PCI INTx above the legacy PIC
 * lines, which occupy GSI 0..15.  Chipsets that override this publish a
 * different base; 0x10 is the ACPI-spec default and the safe assumption for
 * an otherwise unknown chipset. */
#define X86_PC_PCI_INTX_GSI_BASE 0x10U

/* ECAM window resolved from MCFG, kept for enumerate_devices(). */
static uintptr_t pc_ecam_base;
static int pc_bus_start;
static int pc_bus_end;

const char *arch_bootargs_get(void) {
    const char *cmdline = firmware_bootargs();
    if (cmdline[0])
        return cmdline;
    /* No command line at all: a board that booted through GRUB without one
     * gets a DHCP-only configuration rather than a stale QEMU address. */
    return "a20.dhcp=1 a20.hostname=a20os-pc";
}

static void pc_irqchip_init(void) {}

static void pc_irqchip_enable(uint32_t irq) {
    x86_64_pci_irq_set_masked((int)irq, 0);
}

static void pc_irqchip_disable(uint32_t irq) {
    x86_64_pci_irq_set_masked((int)irq, 1);
}

static uint32_t pc_irqchip_ack(void) {
    return 0;
}

static void pc_irqchip_eoi(uint32_t irq) {
    (void)irq;
    lapic_write(LAPIC_EOI, 0);
}

static const irqchip_ops_t pc_irqchip_ops = {
    .init        = pc_irqchip_init,
    .enable_irq  = pc_irqchip_enable,
    .disable_irq = pc_irqchip_disable,
    .ack         = pc_irqchip_ack,
    .eoi         = pc_irqchip_eoi,
};

static uint64_t pc_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t pc_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t pc_timer_ops = {
    .read_ticks    = pc_timer_read_ticks,
    .ticks_per_sec = pc_timer_ticks_per_sec,
};

static unsigned pc_smp_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                                uint64_t boot_hw_id) {
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
}

static int pc_smp_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                        uintptr_t logical_context) {
    return x86_64_smp_start_ap((unsigned)cpu->hw_id, entry_pa,
                               (unsigned)logical_context);
}

static void pc_smp_send(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason) {
    if (reason == SMP_IPI_RESCHEDULE)
        (void)x86_64_smp_send_ipi((unsigned)cpu->hw_id, IRQ_VECTOR_RESCHEDULE);
}

static void pc_smp_secondary(const smp_cpu_desc_t *cpu) {
    (void)cpu;
    x86_64_smp_secondary_init();
}

static const smp_platform_ops_t pc_smp_ops = {
    .discover = pc_smp_discover,
    .start    = pc_smp_start,
    .send_ipi = pc_smp_send,
#if CONFIG_NR_CPUS > 1
    .remote_tlb_flush = x86_64_smp_remote_tlb_flush,
#endif
    .secondary_init = pc_smp_secondary,
};

static void pc_early_init(void) {
    x86_64_enable_fpu_sse();

    pc_ecam_base = firmware_acpi_mcfg_base();
    if (pc_ecam_base) {
        arch_pci_host_init(pc_ecam_base);
        arch_pci_set_intx_gsi_base(X86_PC_PCI_INTX_GSI_BASE);
        uint8_t first = 0, last = 0;
        if (firmware_acpi_mcfg_bus_range(&first, &last) == 0) {
            pc_bus_start = first;
            pc_bus_end   = last;
        } else {
            pc_bus_start = 0;
            pc_bus_end   = 255;
        }
        printf("[PCI] ECAM 0x%lx buses %d..%d intx_gsi_base=0x%x\n",
               (unsigned long)pc_ecam_base, pc_bus_start, pc_bus_end,
               X86_PC_PCI_INTX_GSI_BASE);
    } else {
        /* No MCFG: either a pre-PCIe machine, in which case pci_host.c falls
         * back to the 0xCF8/0xCFC pair, or a firmware that publishes no
         * tables.  Either way the default window is wrong, so probe bus 0. */
        printf("[PCI] no ACPI MCFG; falling back to legacy probing\n");
        pc_bus_start = 0;
        pc_bus_end   = 255;
    }
}

static void pc_poweroff(void) {
    /* ACPI S5 (soft off) via the PM1a control register at the chipset's
     * conventional address, then the QEMU port as a fallback. */
    outw(0x604, 0x2000);
    arch_halt();
}

static void pc_reboot(void) {
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02);
    outb(0x64, 0xFE);
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);
extern int platform_device_register(platform_device_t *pdev);

static void pc_enumerate_devices(void) {
    static resource_t speaker_resources[] = {
        { .type = RES_IOPORT, .start = 0x42, .end = 0x43,
          .name = "pit-channel2" },
        { .type = RES_IOPORT, .start = 0x61, .end = 0x61,
          .name = "speaker-control" },
    };
    static platform_device_t speaker = {
        .dev = {
            .name = "pc-speaker",
            .res = speaker_resources,
            .res_count = 2,
        },
        .id = {
            .vendor = A20_PLATFORM_VENDOR,
            .device = A20_DEVICE_PC_SPEAKER,
        },
    };

    pci_enumerate(pc_ecam_base ? pc_ecam_base : PCI_ECAM_BASE,
                  pc_bus_start, pc_bus_end);
    (void)platform_device_register(&speaker);
}

static const board_config_t x86_64_pc = {
    .name              = "x86_64-pc",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &pc_irqchip_ops,
    .timer             = &pc_timer_ops,
    .smp               = &pc_smp_ops,
    .early_init        = pc_early_init,
    .poweroff          = pc_poweroff,
    .reboot            = pc_reboot,
    .enumerate_devices = pc_enumerate_devices,
};

const board_config_t *const current_board = &x86_64_pc;

#endif /* CONFIG_BOARD_X86_64_PC */
