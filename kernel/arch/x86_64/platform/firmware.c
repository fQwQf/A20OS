#ifdef CONFIG_X86_64

#include "core/types.h"
#include "firmware.h"
#include "cpu.h"
#include "console.h"
#include "platform.h"
#include "core/string.h"

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

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
} __attribute__((packed)) acpi_sdt_t;

static int acpi_checksum(const void *table, size_t length) {
    const uint8_t *bytes = table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += bytes[i];
    return sum == 0;
}

static const acpi_rsdp_t *acpi_find_rsdp_range(uintptr_t start, uintptr_t end) {
    for (uintptr_t pa = (start + 15) & ~15UL; pa + 20 <= end; pa += 16) {
        const acpi_rsdp_t *rsdp = (const void *)(PAGE_OFFSET + pa);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0 &&
            acpi_checksum(rsdp, 20) &&
            (rsdp->revision < 2 ||
             (rsdp->length >= sizeof(*rsdp) && rsdp->length <= 4096 &&
              acpi_checksum(rsdp, rsdp->length))))
            return rsdp;
    }
    return NULL;
}

static const acpi_rsdp_t *acpi_find_rsdp(void) {
    uint16_t ebda_segment = *(volatile uint16_t *)(PAGE_OFFSET + 0x40e);
    uintptr_t ebda = (uintptr_t)ebda_segment << 4;
    const acpi_rsdp_t *rsdp = NULL;
    if (ebda >= 0x400 && ebda < 0xa0000)
        rsdp = acpi_find_rsdp_range(ebda, ebda + 1024);
    return rsdp ? rsdp : acpi_find_rsdp_range(0xe0000, 0x100000);
}

static const acpi_sdt_t *acpi_map_sdt(uint64_t pa) {
    if (!pa || pa > 0xffffffffULL - sizeof(acpi_sdt_t))
        return NULL;
    const acpi_sdt_t *sdt = (const void *)(PAGE_OFFSET + (uintptr_t)pa);
    if (sdt->length < sizeof(*sdt) || sdt->length > 1024 * 1024 ||
        sdt->length > 0x100000000ULL - pa)
        return NULL;
    return acpi_checksum(sdt, sdt->length) ? sdt : NULL;
}

static const acpi_sdt_t *acpi_find_table(const char signature[4]) {
    const acpi_rsdp_t *rsdp = acpi_find_rsdp();
    if (!rsdp)
        return NULL;
    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt;
    const acpi_sdt_t *root = acpi_map_sdt(use_xsdt ? rsdp->xsdt : rsdp->rsdt);
    if (!root || memcmp(root->signature, use_xsdt ? "XSDT" : "RSDT", 4) != 0)
        return NULL;
    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (size_t i = 0; i < count; i++) {
        uint64_t pa = use_xsdt ? ((const uint64_t *)entries)[i]
                               : ((const uint32_t *)entries)[i];
        const acpi_sdt_t *table = acpi_map_sdt(pa);
        if (table && memcmp(table->signature, signature, 4) == 0)
            return table;
    }
    return NULL;
}

size_t firmware_acpi_apic_ids(uint32_t *ids, size_t capacity,
                              uint32_t bsp_apic_id) {
    const acpi_sdt_t *madt = acpi_find_table("APIC");
    if (!ids || !capacity || !madt || madt->length < sizeof(*madt) + 8)
        return 0;

    ids[0] = bsp_apic_id;
    size_t count = 1;
    int found_bsp = 0;
    const uint8_t *entry = (const uint8_t *)madt + sizeof(*madt) + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (entry + 2 <= end && entry[1] >= 2 && entry + entry[1] <= end) {
        uint32_t apic_id = 0;
        uint32_t flags = 0;
        if (entry[0] == 0 && entry[1] >= 8) {
            apic_id = entry[3];
            flags = *(const uint32_t *)(entry + 4);
        } else if (entry[0] == 9 && entry[1] >= 16) {
            apic_id = *(const uint32_t *)(entry + 4);
            flags = *(const uint32_t *)(entry + 8);
        }
        if ((entry[0] == 0 || entry[0] == 9) && (flags & 3) && apic_id <= 255) {
            if (apic_id == bsp_apic_id) {
                found_bsp = 1;
                entry += entry[1];
                continue;
            }
            int duplicate = 0;
            for (size_t i = 0; i < count; i++)
                duplicate |= ids[i] == apic_id;
            if (!duplicate && count < capacity)
                ids[count++] = apic_id;
        }
        entry += entry[1];
    }

    return found_bsp ? count : 0;
}

uintptr_t firmware_acpi_hpet_address(void) {
    const acpi_sdt_t *hpet = acpi_find_table("HPET");
    if (!hpet || hpet->length < sizeof(*hpet) + 20)
        return 0;
    const uint8_t *body = (const uint8_t *)hpet + sizeof(*hpet);
    uint8_t address_space = body[4];
    uint64_t address = *(const uint64_t *)(body + 8);
    if (address_space != 0 || !address || address > 0xffffffffULL - 0x400)
        return 0;
    return PAGE_OFFSET + (uintptr_t)address;
}

void firmware_shutdown(void) {
    outw(0x604, 0x2000);
    arch_halt();
}

void firmware_reboot(void) {
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02);
    outb(0x64, 0xFE);
    arch_halt();
}

void firmware_set_timer(uint64_t time) {
    (void)time;
}

void firmware_console_putchar(char c) {
    arch_uart_putc(c);
}

int firmware_console_getchar(void) {
    return arch_uart_poll_getc();
}

#endif
