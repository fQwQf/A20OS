#ifdef CONFIG_X86_64

#include "core/types.h"
#include "firmware.h"
#include "cpu.h"
#include "console.h"
#include "platform.h"
#include "core/string.h"
#include "core/stdio.h"

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

/* TPM2 ACPI table: returns the physical address of the TPM2 control area
 * (or 0 if absent).  The control-area address points at the tail registers
 * for CRB; TIS uses the base. */
uint64_t firmware_acpi_tpm2(void) {
    const acpi_sdt_t *tpm2 = acpi_find_table("TPM2");
    if (!tpm2 || tpm2->length < 52)
        return 0;
    const uint8_t *body = (const uint8_t *)tpm2 + sizeof(*tpm2);
    uint32_t start_method = *(const uint32_t *)(body + 12);   /* offset 48 */
    uint64_t control = *(const uint64_t *)(body + 4);         /* offset 40 */
    /* start_method: 6 = TIS FIFO, 7/8 = CRB.  Legacy fallback handled by the
     * caller via a 0xFED40000 probe. */
    if (start_method == 6)
        return control ? control : 0xFED40000ULL;
    if (start_method == 7 || start_method == 8)
        return control;
    return 0;
}

void firmware_shutdown(void) {
    outw(0x604, 0x2000);
    arch_halt();
}

/* QEMU fw_cfg (port 0x510 selector / 0x511 data).  With `-kernel`+`-append`
 * QEMU exposes the command line through FW_CFG_CMDLINE_SIZE/DATA; without
 * it, bootargs would be empty on x86_64 and every a20.* knob (static
 * network config, trace=<comm> diagnosis) stays unreachable. */
#define FW_CFG_SELECTOR_PORT 0x510
#define FW_CFG_DATA_PORT     0x511
#define FW_CFG_SIGNATURE     0x0000
#define FW_CFG_CMDLINE_SIZE  0x0014
#define FW_CFG_CMDLINE_DATA  0x0015

static uint8_t fw_cfg_read8(void) {
    return inb(FW_CFG_DATA_PORT);
}

static uint32_t fw_cfg_read32(void) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value = (value << 8) | fw_cfg_read8();
    return value;
}

static char g_fw_cfg_cmdline[256];

const char *firmware_bootargs(void) {
    static int ready;
    if (!ready) {
        ready = 1;
        outw(FW_CFG_SELECTOR_PORT, FW_CFG_SIGNATURE);
        uint32_t sig = fw_cfg_read32();
        printf("[FW_CFG] signature=0x%08x\n", sig);
        if (sig == 0x51454d55U) {   /* "QEMU", big-endian */
            outw(FW_CFG_SELECTOR_PORT, FW_CFG_CMDLINE_SIZE);
            uint32_t len = fw_cfg_read32();
            printf("[FW_CFG] cmdline_size=%u\n", len);
            if (len > sizeof(g_fw_cfg_cmdline) - 1)
                len = sizeof(g_fw_cfg_cmdline) - 1;
            outw(FW_CFG_SELECTOR_PORT, FW_CFG_CMDLINE_DATA);
            for (uint32_t i = 0; i < len; i++)
                g_fw_cfg_cmdline[i] = (char)fw_cfg_read8();
            g_fw_cfg_cmdline[len] = '\0';
            printf("[FW_CFG] cmdline='%s'\n", g_fw_cfg_cmdline);
        } else {
            printf("[FW_CFG] no QEMU fw_cfg, using fallback bootargs\n");
        }
    }
    return g_fw_cfg_cmdline;
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

/*
 * RAM sizing.  boot/entry.S saves the multiboot magic and info pointer (see
 * the comment there) so we can read the real memory map instead of assuming
 * 1 GiB.  Only "available" (type 1) regions above the low 1 MiB are offered
 * (the low page holds the IVT/BDA/legacy hole and must never be handed out),
 * and nothing past the 2 GiB direct-map window that entry.S builds with 1 GiB
 * pages.  If anything is missing we fall back to the old 1 GiB range.
 */
#define X86_MB_BOOTLOADER_MAGIC 0x2BADB002u
#define X86_MB_INFO_MEM_MAP     0x00000040u
#define X86_PAGE_SIZE           4096u
#define X86_RAM_RANGE_MAX       8
#define X86_LOW_RESERVED_END    0x100000ULL
#define X86_DIRECT_MAP_END      0x80000000ULL
#define X86_MB_MMAP_MIN_SIZE    20u

struct x86_mb_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
};

struct x86_mb_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

__attribute__((section(".data"))) volatile uint32_t g_mb_magic;
__attribute__((section(".data"))) volatile uint32_t g_mb_info;

static paddr_t g_ram_base[X86_RAM_RANGE_MAX];
static paddr_t g_ram_end[X86_RAM_RANGE_MAX];
static size_t g_ram_count;
static int g_ram_done;

static void x86_ram_detect(void) {
    g_ram_done = 1;
    g_ram_count = 1;
    g_ram_base[0] = PHYS_MEMORY_BASE;
    g_ram_end[0] = PHYS_MEMORY_END;

    if (g_mb_magic != X86_MB_BOOTLOADER_MAGIC || g_mb_info == 0)
        return;

    const struct x86_mb_info *mi =
        (const struct x86_mb_info *)(uintptr_t)(g_mb_info + PAGE_OFFSET);
    if (!(mi->flags & X86_MB_INFO_MEM_MAP))
        return;

    size_t n = 0;
    uintptr_t p = (uintptr_t)(mi->mmap_addr + PAGE_OFFSET);
    uintptr_t stop = p + mi->mmap_length;
    while (n < X86_RAM_RANGE_MAX && p + X86_MB_MMAP_MIN_SIZE <= stop) {
        const struct x86_mb_mmap_entry *e = (const struct x86_mb_mmap_entry *)p;
        if (e->type == 1 && e->len != 0) {
            paddr_t base = (paddr_t)e->addr;
            paddr_t end = (paddr_t)(e->addr + e->len);
            if (end > X86_DIRECT_MAP_END)
                end = X86_DIRECT_MAP_END;
            if (base < X86_LOW_RESERVED_END)
                base = X86_LOW_RESERVED_END;
            base = (base + X86_PAGE_SIZE - 1) & ~((paddr_t)X86_PAGE_SIZE - 1);
            end &= ~((paddr_t)X86_PAGE_SIZE - 1);
            if (end > base) {
                g_ram_base[n] = base;
                g_ram_end[n] = end;
                n++;
            }
        }
        p += e->size + sizeof(uint32_t);
    }
    if (n == 0)
        return;

    g_ram_count = n;
    for (size_t i = 0; i < n; i++)
        printf("[RAM] usable %p..%p (%lu MiB)\n",
               (void *)g_ram_base[i], (void *)g_ram_end[i],
               (unsigned long)((g_ram_end[i] - g_ram_base[i]) >> 20));
}

size_t arch_ram_range_count(void) {
    if (!g_ram_done)
        x86_ram_detect();
    return g_ram_count;
}

int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (!g_ram_done)
        x86_ram_detect();
    if (idx >= g_ram_count || !base || !end)
        return -1;
    *base = g_ram_base[idx];
    *end = g_ram_end[idx];
    return 0;
}

#endif
