#ifndef _ARCH_X86_64_FIRMWARE_H
#define _ARCH_X86_64_FIRMWARE_H

#include "core/types.h"
#include "firmware/acpi.h"
uint64_t arch_firmware_rsdp_pa(void);
const acpi_context_t *firmware_acpi_context(void);

/* MADT type 1: one I/O APIC.  A machine may have several, and neither their
 * register base nor their share of the global interrupt space is architecturally
 * fixed, so both come from here. */
typedef struct firmware_acpi_ioapic {
    uint32_t ioapic_id;  /* the APIC ID this controller forwards to */
    uint32_t base;       /* physical address of the register window */
    uint32_t gsibase;    /* first global system interrupt it owns */
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
const char *firmware_bootargs(void);

/* Copy up to @max I/O APIC descriptions out of the MADT and return how many the
 * table declares, which can exceed @max so a caller can size a buffer.  Returns
 * 0 when there is no MADT or it lists no I/O APIC, which is the normal case
 * under QEMU's legacy interrupt path. */
size_t firmware_acpi_ioapics(firmware_acpi_ioapic_t *out, size_t max);

/* Request S5 through uACPI (_S5/_PTS/_GTS and fixed/reduced hardware).
 * Halt if firmware does not provide an executable shutdown path. */
void firmware_acpi_poweroff(void);

#endif
