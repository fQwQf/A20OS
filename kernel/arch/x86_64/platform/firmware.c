#ifdef CONFIG_X86_64

#include "core/types.h"
#include "firmware.h"
#include "cpu.h"
#include "console.h"
#include "platform.h"
#include "core/string.h"
#include "core/klog.h"
#include "mm/frame.h"
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

/*
 * An RSDP handed over by the boot path, as a physical address.
 *
 * The two searches below only work under BIOS: the EBDA does not exist under
 * UEFI, and 0xE0000-0x100000 is firmware ROM that OVMF does not publish an RSDP
 * into.  With neither available, acpi_find_table() returned NULL for everything,
 * which is why MCFG was never found, why PCI fell back to a hardcoded ECAM base,
 * and why enumeration then read all-zero vendor IDs and published 129 devices that
 * matched no driver.  The whole chain failed at the first step and the symptom
 * only appeared much later as "no init program found".
 *
 * A UEFI loader can always be asked for the RSDP -- it is in the firmware's
 * configuration table -- so it passes the physical address here.  Zero means the
 * boot path had none to give, which is what BIOS firmware leaves and why the
 * scans below are still needed.
 */
/* Parked by _start_uefi in boot/entry.S from RSI, which is where
 * kernel/boot/uefi/x86_64_loader.c leaves the RSDP it read from the firmware
 * configuration table.  A physical address, like g_mb_info.  It stays zero on a
 * multiboot boot, because nothing sets it there and BSS is cleared. */
extern uint64_t x86_boot_acpi_rsdp;

static uintptr_t g_firmware_rsdp_pa;

/* The second of the two RSDP input channels, for a platform that learns the
 * address from firmware structures rather than from a handover register.  The
 * bootloader path in the tree does not use it -- it writes x86_boot_acpi_rsdp
 * from _start_uefi instead -- so this is an available entry point rather than a
 * wired one.  Nothing here depends on it: firmware_bios_or_uefi() treats a
 * non-zero value as a UEFI boot whichever channel supplied it, and with both
 * zero it falls back to probing. */
void firmware_set_rsdp_pa(uintptr_t pa)
{
    g_firmware_rsdp_pa = pa;
}

static const acpi_rsdp_t *acpi_find_rsdp(void) {
    /* The UEFI loader's handover, checked first: under UEFI there is no EBDA and
     * 0xE0000-0x100000 is firmware ROM, so the BIOS scans below cannot succeed
     * and this is the only address that exists. */
    if (x86_boot_acpi_rsdp) {
        const acpi_rsdp_t *rsdp =
            (const void *)(PAGE_OFFSET + x86_boot_acpi_rsdp);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0)
            return rsdp;
    }

    if (g_firmware_rsdp_pa) {
        const acpi_rsdp_t *rsdp =
            (const void *)(PAGE_OFFSET + g_firmware_rsdp_pa);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0)
            return rsdp;
    }

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

/*
 * MCFG: the PCI Express memory-mapped configuration space allocation.  The
 * base address is a firmware fact and it is not 0xB0000000 on real hardware --
 * that address is QEMU q35's MMCONFIG.  A 2011-onward chipset normally places
 * ECAM at 0xE0000000, and the only reliable way to learn it is this table, so
 * real-hardware boards must read it before pci_enumerate().
 *
 *
 * MCFG ships in two incompatible published layouts, and firmware uses both.
 *
 * PCI Firmware Specification r3.0 -- what SeaBIOS emits -- lays the allocation
 * subtable out as { u64 address; u16 segment; u8 start_bus; u8 end_bus; u32
 * reserved; }, which puts the base address at 44 and makes the whole table
 * exactly 60 bytes.  The ACPI specification instead reserves a u64 at 44 and
 * moves the base address out to 59, behind { u8 segment; u8 start_bus; u8
 * end_bus; u32 reserved; }.
 *
 * Measured on SeaBIOS (q35, 1 GiB): length 60, base 0xb0000000 read from 44,
 * end bus 0xff at 55, and offset 59 already holding the next table's signature.
 * That last detail is why the layouts are told apart by length rather than by
 * guesswork -- a 60-byte table cannot be the ACPI one.
 */
#define ACPI_MCFG_OFF_PCI_BASE      44u
#define ACPI_MCFG_OFF_PCI_SEGMENT   52u
#define ACPI_MCFG_OFF_PCI_START     54u
#define ACPI_MCFG_OFF_PCI_END       55u
#define ACPI_MCFG_MIN_PCI_LEN       56u

#define ACPI_MCFG_OFF_ACPI_SEGMENT  52u
#define ACPI_MCFG_OFF_ACPI_START    53u
#define ACPI_MCFG_OFF_ACPI_END      54u
#define ACPI_MCFG_OFF_ACPI_BASE     59u
#define ACPI_MCFG_MIN_ACPI_LEN      67u

/* ECAM is a 256 MiB-aligned window below 4 GiB by definition. */
#define ACPI_MCFG_BASE_MAX          0xffffffffULL
#define ACPI_MCFG_BASE_ALIGN_MASK   0x0fffffffULL

/* Only segment 0 is mapped by the boot page tables; a non-zero segment would
 * need a real mapping rather than PAGE_OFFSET arithmetic. */
#define ACPI_MCFG_BASE_SEGMENT      0u

struct acpi_mcfg_view {
    uint64_t base;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
};

static uint64_t mcfg_read_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/*
 * Resolve this MCFG into base/segment/bus range, or -1 if it is neither layout.
 *
 * The ACPI layout is tried first because it is the longer one, so a table that
 * is long enough to be ACPI is not misread as PCI.  Each candidate then has to
 * survive the ECAM sanity check, which is a real filter rather than a formality:
 * reading the base from the wrong offset lands on the reserved field or on the
 * neighbouring table, and neither is 256 MiB aligned.
 */
static int acpi_mcfg_view(const acpi_sdt_t *mcfg, struct acpi_mcfg_view *out)
{
    const uint8_t *b = (const uint8_t *)mcfg;

    if (mcfg->length >= ACPI_MCFG_MIN_ACPI_LEN) {
        out->base      = mcfg_read_le64(b + ACPI_MCFG_OFF_ACPI_BASE);
        out->segment   = b[ACPI_MCFG_OFF_ACPI_SEGMENT];
        out->start_bus = b[ACPI_MCFG_OFF_ACPI_START];
        out->end_bus   = b[ACPI_MCFG_OFF_ACPI_END];
        if (out->base && !(out->base & ACPI_MCFG_BASE_ALIGN_MASK) &&
            out->base <= ACPI_MCFG_BASE_MAX)
            return 0;
    }

    if (mcfg->length >= ACPI_MCFG_MIN_PCI_LEN) {
        out->base      = mcfg_read_le64(b + ACPI_MCFG_OFF_PCI_BASE);
        out->segment   = (uint16_t)b[ACPI_MCFG_OFF_PCI_SEGMENT] |
                         ((uint16_t)b[ACPI_MCFG_OFF_PCI_SEGMENT + 1] << 8);
        out->start_bus = b[ACPI_MCFG_OFF_PCI_START];
        out->end_bus   = b[ACPI_MCFG_OFF_PCI_END];
        if (out->base && !(out->base & ACPI_MCFG_BASE_ALIGN_MASK) &&
            out->base <= ACPI_MCFG_BASE_MAX)
            return 0;
    }

    return -1;
}

/*
 * "BIOS" or "UEFI".
 *
 * The distinction decides whether a missing MCFG is normal or fatal: under
 * SeaBIOS the RSDP is in a region the legacy search already covers, while under
 * UEFI it is not, so a missing MCFG there means the RSDP was never found and the
 * fallback window holds nothing.
 *
 * A boot path that hands us an RSDP address settles it outright.  There is no
 * such handover under BIOS -- no firmware is told where the table is, because
 * nothing goes looking -- so a non-zero address can only have come from a
 * UEFI-aware loader: x86_boot_acpi_rsdp from _start_uefi, or g_firmware_rsdp_pa
 * from firmware_set_rsdp_pa().  That makes the answer a fact rather than a
 * guess, which matters now that the UEFI path really does reach this code and
 * the old probe would have called it "BIOS" because the loader made the table
 * findable.
 *
 * With no handover to go on, fall back to the probe.  It can only be wrong for
 * a UEFI boot that found its RSDP by scanning, which is a firmware that does
 * both.
 */
const char *firmware_bios_or_uefi(void)
{
    if (x86_boot_acpi_rsdp || g_firmware_rsdp_pa)
        return "UEFI";
    return acpi_find_rsdp() ? "BIOS" : "UEFI";
}

uintptr_t firmware_acpi_mcfg_base(void) {
    const acpi_sdt_t *mcfg = acpi_find_table("MCFG");
    if (!mcfg)
        return 0;
    struct acpi_mcfg_view v;
    if (acpi_mcfg_view(mcfg, &v) != 0 || v.segment != ACPI_MCFG_BASE_SEGMENT)
        return 0;
    /* Direct-mapped, not physical: the caller dereferences this directly, and
     * dropping PAGE_OFFSET points the PCI host at unmapped memory. */
    return PAGE_OFFSET + (uintptr_t)v.base;
}

int firmware_acpi_mcfg_bus_range(uint8_t *start_bus, uint8_t *end_bus) {
    if (!start_bus || !end_bus)
        return -1;
    const acpi_sdt_t *mcfg = acpi_find_table("MCFG");
    if (!mcfg)
        return -1;
    struct acpi_mcfg_view v;
    if (acpi_mcfg_view(mcfg, &v) != 0)
        return -1;
    *start_bus = v.start_bus;
    *end_bus   = v.end_bus;
    return 0;
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

/* Read a fixed-width field out of an ACPI table body, bounds-checked.
 *
 * The tables this file reads are firmware-supplied and the header length is
 * firmware-supplied too, so every field read has to ask whether the bytes it
 * wants are actually inside the table.  A short table is not an error worth
 * reporting: it just means the field is absent, which is how the caller learns
 * the answer is 0. */
static uint64_t acpi_field(const acpi_sdt_t *table, size_t offset, size_t width) {
    if (!table || offset + width > table->length)
        return 0;
    const uint8_t *p = (const uint8_t *)table + offset;
    uint64_t value = 0;
    for (size_t i = 0; i < width; i++)
        value |= (uint64_t)p[i] << (i * 8);
    return value;
}

/* Hand out a mapped ACPI table body, so a parser can walk one without
 * re-implementing RSDP discovery and the checksum walk.  @sig is a four
 * character signature.  Returns NULL when the machine published no such table.
 *
 * The returned pointer is in the direct map and stays valid for the life of the
 * kernel: the tables are firmware ROM, and nothing here remaps or copies them.
 * The length excludes the header, so a caller adding its own offset arithmetic
 * to the body cannot run past the table. */
const void *firmware_acpi_table(const char sig[4], uint32_t *length) {
    const acpi_sdt_t *sdt = acpi_find_table(sig);
    if (!sdt)
        return NULL;
    if (length)
        *length = sdt->length - (uint32_t)sizeof(*sdt);
    return (const uint8_t *)sdt + sizeof(*sdt);
}

int firmware_acpi_pm_get(firmware_acpi_pm_t *out) {
    if (!out)
        return -EINVAL;
    memset(out, 0, sizeof(*out));

    /* "FACP" is the ACPI 2.0-and-later name for this table; revision 1 firmware
     * called it "FADT".  Both are in the field and a machine may publish either,
     * so try both rather than reporting no power-management support to hardware
     * that has it. */
    const acpi_sdt_t *fadt = acpi_find_table("FACP");
    if (!fadt)
        fadt = acpi_find_table("FADT");
    if (!fadt)
        return -ENOENT;

    /* Offsets are absolute from the start of the table (ACPI 6.4 section 5.2.5).
     * The two block bases live at 44 and 52; everything else below is the
     * post-revision-2 restatement of the length fields, read only when the
     * table is long enough to carry it. */
    out->pm1a_evt    = (uint16_t)acpi_field(fadt, 44, 4);
    out->pm1a_cnt    = (uint16_t)acpi_field(fadt, 52, 4);
    out->pm2_cnt     = (uint16_t)acpi_field(fadt, 60, 4);
    out->pm1a_evt_len = (uint8_t)acpi_field(fadt, 76, 1);
    out->pm1a_cnt_len = (uint8_t)acpi_field(fadt, 77, 1);
    out->pm2_cnt_len  = (uint8_t)acpi_field(fadt, 78, 1);

    if (fadt->revision >= 2) {
        out->gpe0_blk    = (uint16_t)acpi_field(fadt, 68, 4);
        out->gpe1_blk    = (uint16_t)acpi_field(fadt, 72, 4);
        out->sleep_states = (uint32_t)acpi_field(fadt, 116, 4);
    }

    /* A width the spec does not define is a corrupt field, not something to
     * guess at: a mis-guessed access size to the sleep register is a write to
     * whatever register is next to it.  Fall back to the only widths that have
     * ever existed, and require the block to be addressable at all. */
    if (out->pm1a_cnt_len != 2 && out->pm1a_cnt_len != 4)
        out->pm1a_cnt_len = 2;
    if (out->pm1a_evt_len != 2 && out->pm1a_evt_len != 4 && out->pm1a_evt_len != 8)
        out->pm1a_evt_len = 2;
    if (out->pm2_cnt_len != 1 && out->pm2_cnt_len != 2 && out->pm2_cnt_len != 4)
        out->pm2_cnt_len = 1;

    out->valid = out->pm1a_cnt != 0;
    return 0;
}

/* One I/O APIC, MADT type 1.  The entry is 8 bytes through GSI Base and grows to
 * 12 with the GSI segment number, which only exists to name a range outside
 * segment 0 -- a range this kernel cannot reach, so those entries are dropped
 * rather than silently treated as if they were in segment 0. */
size_t firmware_acpi_ioapics(firmware_acpi_ioapic_t *out, size_t max) {
    const acpi_sdt_t *madt = acpi_find_table("APIC");
    if (!madt || madt->length < sizeof(*madt) + 10)
        return 0;

    size_t total = 0;
    const uint8_t *entry = (const uint8_t *)madt + sizeof(*madt) + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (entry + 2 <= end && entry[1] >= 2 && entry + entry[1] <= end) {
        if (entry[0] == 1 && entry[1] >= 12) {
            uint32_t segment = (entry[1] >= 16) ? *(const uint32_t *)(entry + 12) : 0;
            if (segment == 0) {
                if (out && total < max) {
                    out[total].ioapic_id = entry[2];
                    out[total].base      = *(const uint32_t *)(entry + 4);
                    out[total].gsibase   = *(const uint32_t *)(entry + 8);
                    out[total].gsiseg    = segment;
                }
                total++;
            }
        }
        entry += entry[1];
    }
    return total;
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

/* PM1a_CNT: sleep type in bits 12:10, the enable that latches it in bit 13.
 * S5 (soft off) is sleep type 5 (ACPI 6.4 table 5-383). */
#define PM1A_CNT_SLP_TYP_S5 5U
#define PM1A_CNT_SLP_EN     (1U << 13)

void firmware_acpi_poweroff(void) {
    firmware_acpi_pm_t pm;
    if (firmware_acpi_pm_get(&pm) != 0 || !pm.valid) {
        /* No FADT, or one that names no PM1a_CNT block.  There is no address to
         * ask, and the port this kernel used to write blind is QEMU's PIIX4
         * default rather than anything the architecture promises, so stopping
         * here is the only write that cannot land on an unrelated register. */
        arch_halt();
        return;
    }

    uint32_t s5 = (PM1A_CNT_SLP_TYP_S5 << 10) | PM1A_CNT_SLP_EN;
    if (pm.pm1a_cnt_len == 4)
        outl(pm.pm1a_cnt, s5);
    else
        outw(pm.pm1a_cnt, (uint16_t)s5);

    /* The conformant encoding above is all a real chipset needs, and on one the
     * machine is gone before the next statement runs.  QEMU is not conformant
     * here: hw/acpi/core.c acpi_pm_cnt_write() reads the sleep type, sends type 0
     * straight to shutdown, sends type 1 to suspend, and treats everything else
     * as an S4 request that only fires when the type matches the machine's s4_val
     * (2 by default).  Type 5 therefore falls through and does nothing there,
     * which would hang every QEMU gate that halts the guest.  So the legacy
     * encoding is written as well, after a bounded spin: hardware has already
     * stopped before reaching it, and QEMU reaches it and stops. */
    for (volatile unsigned spin = 0; spin < 4096U; spin++)
        __asm__ __volatile__("pause");

    if (pm.pm1a_cnt_len == 4)
        outl(pm.pm1a_cnt, PM1A_CNT_SLP_EN);
    else
        outw(pm.pm1a_cnt, (uint16_t)PM1A_CNT_SLP_EN);

    arch_halt();
}

void firmware_shutdown(void) {
    firmware_acpi_poweroff();
}

/* QEMU fw_cfg (port 0x510 selector / 0x511 data).
 *
 * The command line is *not* reachable through the old fixed keys
 * FW_CFG_CMDLINE_SIZE/DATA (0x14/0x15): those belong to the Linux boot
 * protocol and QEMU only fills them in when it boots a bzImage through a
 * setup header.  Under `-kernel` multiboot they read back as zero, which is
 * why bootargs used to come out empty here and every a20.* knob -- static
 * network config, trace=<comm> -- was unreachable on x86_64.
 *
 * What QEMU actually publishes is the file directory: every `-fw_cfg
 * name=...,file/string=...` item gets a selector and a 64-byte record in a
 * flat table at FW_CFG_FILE_DIR, so `-fw_cfg name=opt/x86/cmdline,string=...`
 * is a supported way to hand the kernel a command line.  Read the directory,
 * find the name, then read the file through the selector its record names. */
#define FW_CFG_SELECTOR_PORT 0x510
#define FW_CFG_DATA_PORT     0x511
#define FW_CFG_SIGNATURE     0x0000
#define FW_CFG_FILE_DIR      0x0019
#define FW_CFG_FILE_NAME_LEN 56
#define FW_CFG_FILE_REC_LEN  64
#define FW_CFG_FILE_MAX      256
#define FW_CFG_CMDLINE_FILE  "opt/x86/cmdline"

static uint8_t fw_cfg_read8(void) {
    return inb(FW_CFG_DATA_PORT);
}

static uint32_t fw_cfg_read32(void) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value = (value << 8) | fw_cfg_read8();
    return value;
}

/* Everything in fw_cfg is big-endian and the data port is one sequential
 * stream per selected item: writing the selector rewinds to that item, and
 * each following read just keeps walking forward. */
static uint16_t fw_cfg_read16(void) {
    return (uint16_t)((fw_cfg_read8() << 8) | fw_cfg_read8());
}

/* Copy the fw_cfg file called `name` into `out`.  The directory is a
 * big-endian entry count followed by that many fixed 64-byte records of
 * {u32 size, u16 select, u16 reserved, char name[56]}; a file's bytes are
 * then read straight from the selector its record carries, with no length
 * prefix.  Returns 0 on success. */
static int fw_cfg_read_file(const char *name, char *out, size_t outsz) {
    outw(FW_CFG_SELECTOR_PORT, FW_CFG_FILE_DIR);
    uint32_t count = fw_cfg_read32();
    /* No directory at all, or a count no real machine would publish: both mean
     * there is nothing named `name`, and guessing past that would feed
     * whatever the port happens to return to every bootargs consumer. */
    if (count == 0 || count > FW_CFG_FILE_MAX)
        return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = fw_cfg_read32();
        uint16_t select = fw_cfg_read16();
        (void)fw_cfg_read16();        /* reserved */
        char entry[FW_CFG_FILE_NAME_LEN];
        for (int n = 0; n < FW_CFG_FILE_NAME_LEN; n++)
            entry[n] = (char)fw_cfg_read8();
        entry[FW_CFG_FILE_NAME_LEN - 1] = '\0';

        if (strcmp(entry, name) != 0)
            continue;
        if (size >= outsz)
            size = outsz - 1;
        outw(FW_CFG_SELECTOR_PORT, select);
        for (uint32_t k = 0; k < size; k++)
            out[k] = (char)fw_cfg_read8();
        out[size] = '\0';
        return 0;
    }
    return -1;
}

static char g_bootargs[256];

#define MULTIBOOT_TAG_ACPI_OLD 14u

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

/* Defined further down with the rest of the multiboot state. */
extern __attribute__((section(".data"))) volatile uint32_t g_mb_magic;
extern __attribute__((section(".data"))) volatile uint32_t g_mb_info;

/* Multiboot v1 passes the kernel command line as a physical address in the
 * info block (offset 16).  This is the only source of bootargs on real
 * hardware: GRUB hands the string over in the multiboot info whether it was
 * loaded by BIOS or by UEFI boot services, whereas fw_cfg is a QEMU device that
 * simply does not exist on a physical machine.  Reading only fw_cfg left every
 * a20.* knob unreachable on real x86_64. */
static const char *multiboot_cmdline(void) {
    if (g_mb_magic != 0x2BADB002u || !g_mb_info)
        return NULL;
    /* g_mb_info is physical (EBX) and cannot hold a direct-mapped address: it is
     * a uint32_t and PAGE_OFFSET does not fit in 32 bits.  So every dereference
     * adds PAGE_OFFSET itself, as x86_ram_detect() does; a raw read works only
     * because entry.S identity-maps the first gigabyte. */
    const struct x86_mb_info *mi =
        (const void *)(PAGE_OFFSET + (uintptr_t)g_mb_info);
    if (!mi->cmdline)
        return NULL;
    return (const char *)(PAGE_OFFSET + (uintptr_t)mi->cmdline);
}

const char *firmware_bootargs(void) {
    static int mb_probed;
    if (!mb_probed) {
        mb_probed = 1;
        /* Do this before anything asks about ACPI: the RSDP is what makes MCFG,
         * MADT and the rest reachable, and on UEFI there is nowhere else to look
         * for it. */
        const char *mb = multiboot_cmdline();
        if (mb) {
            size_t i = 0;
            for (; i + 1 < sizeof(g_bootargs) && mb[i]; i++)
                g_bootargs[i] = mb[i];
            g_bootargs[i] = '\0';
            printf("[BOOTARGS] multiboot cmdline='%s'\n", g_bootargs);
        }
    }
    if (g_bootargs[0])
        return g_bootargs;

    static int fw_probed;
    if (!fw_probed) {
        fw_probed = 1;
        outw(FW_CFG_SELECTOR_PORT, FW_CFG_SIGNATURE);
        uint32_t sig = fw_cfg_read32();
        printf("[FW_CFG] signature=0x%08x\n", sig);
        if (sig == 0x51454d55U &&   /* "QEMU", big-endian */
            fw_cfg_read_file(FW_CFG_CMDLINE_FILE, g_bootargs, sizeof(g_bootargs)) == 0)
            printf("[FW_CFG] %s='%s'\n", FW_CFG_CMDLINE_FILE, g_bootargs);
        else
            printf("[FW_CFG] no %s, using fallback bootargs\n", FW_CFG_CMDLINE_FILE);
    }
    return g_bootargs;
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

/*
 * QEMU puts a large guest's second half above 4 GiB (with -m 4G: 0-2 GiB below
 * and 4-6 GiB above), and nothing of the kernel's MMIO lives up there: the PCI
 * ECAM and the MMIO window are at 2.75/3 GiB, inside the uncacheable part of
 * the boot map.  The boot map stops at 4 GiB, so map the whole 1 GiB chunks a
 * usable range fully covers, as cacheable RAM, and report them as usable.
 * Chunks are all-or-nothing on purpose: a partially RAM 1 GiB page would also
 * make whatever else is in it cacheable.
 */
#define X86_HIGH_RAM_BASE       0x100000000ULL
#define X86_HIGH_PAGE           0x40000000ULL

extern uint64_t boot_pdpt_hh[512];

static void x86_high_ram_map_flush(void)
{
    __asm__ __volatile__(
        "movq %%cr3, %%rax\n\t"
        "movq %%rax, %%cr3\n\t"
        ::: "rax", "memory");
}

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
    int mapped_high = 0;
    uintptr_t p = (uintptr_t)(mi->mmap_addr + PAGE_OFFSET);
    uintptr_t stop = p + mi->mmap_length;
    while (p + X86_MB_MMAP_MIN_SIZE <= stop) {
        const struct x86_mb_mmap_entry *e = (const struct x86_mb_mmap_entry *)p;
        if (e->type == 1 && e->len != 0) {
            paddr_t raw_base = (paddr_t)e->addr;
            paddr_t raw_end = (paddr_t)(e->addr + e->len);
            if (raw_base < X86_LOW_RESERVED_END)
                raw_base = X86_LOW_RESERVED_END;

            paddr_t base = (raw_base + X86_PAGE_SIZE - 1) &
                           ~((paddr_t)X86_PAGE_SIZE - 1);
            paddr_t end = raw_end;
            if (end > X86_DIRECT_MAP_END)
                end = X86_DIRECT_MAP_END;
            end &= ~((paddr_t)X86_PAGE_SIZE - 1);
            if (end > base && n < X86_RAM_RANGE_MAX) {
                g_ram_base[n] = base;
                g_ram_end[n] = end;
                n++;
            }

            paddr_t chunk = raw_base > X86_HIGH_RAM_BASE ?
                            (raw_base + X86_HIGH_PAGE - 1) &
                                ~(X86_HIGH_PAGE - 1)
                            : X86_HIGH_RAM_BASE;
            for (; chunk + X86_HIGH_PAGE <= raw_end &&
                   chunk + X86_HIGH_PAGE <= X86_HIGH_RAM_MAP_END &&
                   n < X86_RAM_RANGE_MAX;
                 chunk += X86_HIGH_PAGE) {
                boot_pdpt_hh[chunk >> 30] = (uint64_t)chunk | 0x83ULL;
                mapped_high = 1;
                g_ram_base[n] = chunk;
                g_ram_end[n] = chunk + X86_HIGH_PAGE;
                n++;
            }
        }
        if (p + e->size + sizeof(uint32_t) <= p)
            break;
        p += e->size + sizeof(uint32_t);
    }

    if (n == 0)
        return;

    /*
     * Coalesce before handing the map over.
     *
     * A firmware multiboot map is not a list of the memory you may use, it is a
     * list of everything the firmware found, so it arrives full of adjacent and
     * sub-page pieces.  GRUB on a UEFI VM reports seven entries, and the PFA
     * accepts at most PFA_MAX_RANGES (4), so appending entries verbatim made
     * pfa_init() panic with "invalid ram range count" -- the x86_64 port could
     * not boot from real firmware at all, only from QEMU's -kernel map, which
     * conveniently reports exactly one range.
     *
     * Merging neighbours is not a workaround for the cap, it is what the list
     * means: two entries that touch describe one usable region, and the PFA is
     * better served by one range than by two.  Sorting first is what makes the
     * merge possible, since the firmware is under no obligation to order them.
     */
    for (size_t i = 1; i < n; i++) {      /* insertion sort: n <= 8 */
        paddr_t b = g_ram_base[i], e = g_ram_end[i];
        size_t j = i;
        while (j > 0 && g_ram_base[j - 1] > b) {
            g_ram_base[j] = g_ram_base[j - 1];
            g_ram_end[j] = g_ram_end[j - 1];
            j--;
        }
        g_ram_base[j] = b;
        g_ram_end[j] = e;
    }

    size_t merged = 0;
    for (size_t i = 0; i < n; i++) {
        if (merged > 0 && g_ram_base[i] <= g_ram_end[merged - 1]) {
            if (g_ram_end[i] > g_ram_end[merged - 1])
                g_ram_end[merged - 1] = g_ram_end[i];
            continue;
        }
        g_ram_base[merged] = g_ram_base[i];
        g_ram_end[merged] = g_ram_end[i];
        merged++;
    }
    n = merged;

    /*
     * Still more ranges than the PFA has room for.  Merging cannot fix that -- the
     * gaps are real -- so keep the largest, which is the memory that matters, and
     * say so rather than dropping the excess silently.
     */
    if (n > PFA_MAX_RANGES) {
        kinfo("[RAM] %zu ranges after merge, PFA takes %d; keeping the largest\n",
              n, PFA_MAX_RANGES);
        size_t best = 0;
        for (size_t i = 1; i < n; i++)
            if (g_ram_end[i] - g_ram_base[i] >
                g_ram_end[best] - g_ram_base[best])
                best = i;
        paddr_t b = g_ram_base[best], e = g_ram_end[best];
        g_ram_base[0] = b;
        g_ram_end[0] = e;
        n = 1;
    }

    g_ram_count = n;
    for (size_t i = 0; i < n; i++)
        printf("[RAM] usable %p..%p (%lu MiB)\n",
               (void *)g_ram_base[i], (void *)g_ram_end[i],
               (unsigned long)((g_ram_end[i] - g_ram_base[i]) >> 20));
    if (mapped_high)
        x86_high_ram_map_flush();
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
