#ifdef CONFIG_AARCH64

#include "drivers/core/driver_core.h"
#include "drivers/bus/pci_bus.h"
#include "core/arch.h"
#include "core/timer.h"
#include "core/stdio.h"

extern uint64_t aarch64_boot_acpi_rsdp;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    acpi_sdt_header_t header;
    uint64_t reserved;
} __attribute__((packed)) acpi_mcfg_t;

typedef struct {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_allocation_t;

static int vbox_acpi_checksum_ok(const void *table, size_t length) {
    const uint8_t *p = (const uint8_t *)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static acpi_sdt_header_t *vbox_acpi_find_table(const char signature[4]) {
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)(uintptr_t)
        (aarch64_boot_acpi_rsdp + PAGE_OFFSET);
    if (!rsdp || !vbox_acpi_checksum_ok(rsdp, 20))
        return NULL;

    /* UEFI is allowed to publish either ACPI 1.0 or ACPI 2.0.  Prefer the
     * XSDT when it is available, but retain RSDT support: rejecting revision
     * 0 was making PCI discovery depend on an unnecessary firmware detail. */
    int entry_size;
    acpi_sdt_header_t *root;
    if (rsdp->revision >= 2 && rsdp->length >= sizeof(*rsdp) &&
        vbox_acpi_checksum_ok(rsdp, rsdp->length) && rsdp->xsdt_address) {
        root = (acpi_sdt_header_t *)(uintptr_t)(rsdp->xsdt_address + PAGE_OFFSET);
        entry_size = 8;
    } else if (rsdp->rsdt_address) {
        root = (acpi_sdt_header_t *)(uintptr_t)((uint64_t)rsdp->rsdt_address + PAGE_OFFSET);
        entry_size = 4;
    } else {
        return NULL;
    }

    if (root->length < sizeof(*root) || root->length > (1U << 20) ||
        !vbox_acpi_checksum_ok(root, root->length))
        return NULL;

    size_t entries = (root->length - sizeof(*root)) / (size_t)entry_size;
    const uint8_t *entry = (const uint8_t *)(root + 1);
    for (size_t i = 0; i < entries; i++) {
        uint64_t table_address = entry_size == 8
            ? ((const uint64_t *)entry)[i] : ((const uint32_t *)entry)[i];
        acpi_sdt_header_t *table = (acpi_sdt_header_t *)(uintptr_t)
            (table_address + PAGE_OFFSET);
        if (!table || table->length < sizeof(*table) || table->length > (1U << 20) ||
            !vbox_acpi_checksum_ok(table, table->length))
            continue;
        if (table->signature[0] == signature[0] && table->signature[1] == signature[1] &&
            table->signature[2] == signature[2] && table->signature[3] == signature[3])
            return table;
    }
    return NULL;
}

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

/* The UEFI handoff currently has no command-line channel.  Use VBox NAT's
 * documented guest subnet directly so networking does not depend on timer
 * driven DHCP retries while this board still uses its software timer. */
const char *arch_bootargs_get(void) {
    return "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
           "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 "
           "a20.hostname=a20os-vbox";
}

static void vbox_aa64_poweroff(void) {
    sbi_shutdown();
}

static void vbox_aa64_reboot(void) {
    sbi_reboot();
}

static void vbox_aa64_enumerate_devices(void) {
    printf("[VBOX] ACPI RSDP: 0x%lx\n", (unsigned long)aarch64_boot_acpi_rsdp);
    acpi_mcfg_t *mcfg = (acpi_mcfg_t *)vbox_acpi_find_table("MCFG");
    if (!mcfg || mcfg->header.length < sizeof(*mcfg)) {
        printf("[VBOX] ACPI MCFG unavailable (RSDP=0x%lx)\n",
               (unsigned long)aarch64_boot_acpi_rsdp);
        return;
    }

    size_t count = (mcfg->header.length - sizeof(*mcfg)) /
                   sizeof(acpi_mcfg_allocation_t);
    if (count == 0) {
        printf("[VBOX] ACPI MCFG has no PCI segments\n");
        return;
    }
    acpi_mcfg_allocation_t *alloc = (acpi_mcfg_allocation_t *)(mcfg + 1);
    int enumerated = 0;
    for (size_t i = 0; i < count; i++) {
        if (alloc[i].segment_group != 0 || !alloc[i].base_address ||
            alloc[i].end_bus < alloc[i].start_bus)
            continue;
        if (alloc[i].base_address >= 0x100000000ULL) {
            printf("[VBOX] PCIe ECAM above bootstrap map: 0x%lx\n",
                   (unsigned long)alloc[i].base_address);
            continue;
        }
        uintptr_t ecam = (uintptr_t)alloc[i].base_address + PAGE_OFFSET;
        printf("[VBOX] PCIe ECAM: 0x%lx, buses %u-%u\n", (unsigned long)ecam,
               alloc[i].start_bus, alloc[i].end_bus);
        pci_enumerate(ecam, alloc[i].start_bus, (int)alloc[i].end_bus + 1);
        enumerated = 1;
        break; /* A20OS currently has one PCI segment. */
    }
    if (!enumerated)
        printf("[VBOX] no usable PCIe ECAM allocation in MCFG\n");
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
