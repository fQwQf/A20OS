#ifndef _ARCH_X86_64_FIRMWARE_H
#define _ARCH_X86_64_FIRMWARE_H

#include "core/types.h"

/*
 * FADT (published under the signature "FACP"): the addresses of the fixed ACPI
 * hardware registers.  Every field here is a firmware fact with no architectural
 * default -- the power-management base in particular is a chipset property, and
 * the value this kernel used to hardcode (0x604) is QEMU's PIIX4 default rather
 * than a convention.  Reading the table is therefore the only way to reach the
 * sleep registers on real hardware.
 */
typedef struct firmware_acpi_pm {
    /* I/O ports.  0 means the table did not publish one, which is legitimate:
     * a platform may have no PM2 block at all. */
    uint16_t pm1a_evt;
    uint16_t pm1a_cnt;
    uint16_t pm2_cnt;
    uint16_t gpe0_blk;
    uint16_t gpe1_blk;
    /* Register widths in bytes.  PM1a_CNT is 2 or 4; the access size matters
     * because a 4-byte register must not be written a half at a time. */
    uint8_t  pm1a_evt_len;
    uint8_t  pm1a_cnt_len;
    uint8_t  pm2_cnt_len;
    /* A chipset bitmask of the sleep states it implements (ACPI 6.4 FACP
     * offset 116).  Bit n set means _Sn exists.  Zero when unknown. */
    uint32_t sleep_states;
    /* Set when the FADT was found and carries a usable PM1a_CNT block. */
    int      valid;
} firmware_acpi_pm_t;

/* MADT type 1: one I/O APIC.  A machine may have several, and neither their
 * register base nor their share of the global interrupt space is architecturally
 * fixed, so both come from here. */
typedef struct firmware_acpi_ioapic {
    uint32_t ioapic_id;  /* the APIC ID this controller forwards to */
    uint32_t base;       /* physical address of the register window */
    uint32_t gsibase;    /* first global system interrupt it owns */
    uint32_t gsiseg;     /* segment the GSI range lives in; only 0 is mapped */
} firmware_acpi_ioapic_t;

void firmware_shutdown(void);
void firmware_reboot(void);
void firmware_set_timer(uint64_t time);
void firmware_console_putchar(char c);
int  firmware_console_getchar(void);
size_t firmware_acpi_apic_ids(uint32_t *ids, size_t capacity,
                              uint32_t bsp_apic_id);
/* ECAM base and bus range from the ACPI MCFG table.  Both return 0 / -1 when
 * no usable MCFG exists, which is the normal case under QEMU and the only way
 * to reach a real chipset's configuration space. */
uintptr_t firmware_acpi_mcfg_base(void);
/* "BIOS" or "UEFI": whether a boot path handed over an RSDP address.  That only
 * happens under UEFI, so when one was given it is the answer rather than an
 * inference; otherwise it falls back to whether the legacy search succeeds.  A
 * missing MCFG is normal under BIOS and fatal under UEFI, so callers need to
 * tell them apart. */
const char *firmware_bios_or_uefi(void);
/* Physical address of an RSDP the boot path was given, or 0.  Only a UEFI-aware
 * loader has one to give.  Checked before the BIOS-region scans, which cannot
 * see a table under UEFI. */
void firmware_set_rsdp_pa(uintptr_t pa);
int firmware_acpi_mcfg_bus_range(uint8_t *start_bus, uint8_t *end_bus);
uintptr_t firmware_acpi_hpet_address(void);
uint64_t firmware_acpi_tpm2(void);
const char *firmware_bootargs(void);

/* Fill @out from the FADT.  Returns 0 when a FADT was found, -ENOENT when the
 * machine published none.  @out->valid distinguishes a FADT that exists but
 * carries no PM1a_CNT block from one that does -- only the latter can shut the
 * machine down through ACPI. */
int firmware_acpi_pm_get(firmware_acpi_pm_t *out);

/* A mapped ACPI table body: the bytes after the header, in the direct map, with
 * @length set to the body size.  NULL when the machine published no table with
 * that four-character signature.  The pointer stays valid for the life of the
 * kernel and must not be freed. */
const void *firmware_acpi_table(const char sig[4], uint32_t *length);

/* Copy up to @max I/O APIC descriptions out of the MADT and return how many the
 * table declares, which can exceed @max so a caller can size a buffer.  Returns
 * 0 when there is no MADT or it lists no I/O APIC, which is the normal case
 * under QEMU's legacy interrupt path. */
size_t firmware_acpi_ioapics(firmware_acpi_ioapic_t *out, size_t max);

/* Request S5 (soft off) through the ACPI PM1a control register.  Falls back to
 * halt() when the machine published no FADT, so a caller never has to branch on
 * firmware it did not ask about. */
void firmware_acpi_poweroff(void);

#endif
