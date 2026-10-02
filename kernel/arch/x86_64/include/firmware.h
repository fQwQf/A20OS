#ifndef _ARCH_X86_64_FIRMWARE_H
#define _ARCH_X86_64_FIRMWARE_H

#include "core/types.h"

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
/* "BIOS" or "UEFI": whether the legacy RSDP search succeeds.  A missing MCFG is
 * normal under BIOS and fatal under UEFI, so callers need to tell them apart. */
const char *firmware_bios_or_uefi(void);
/* Physical address of an RSDP the boot path was given, or 0.  Checked before the
 * BIOS-region scans, which cannot see one under UEFI. */
void firmware_set_rsdp_pa(uintptr_t pa);
int firmware_acpi_mcfg_bus_range(uint8_t *start_bus, uint8_t *end_bus);
uintptr_t firmware_acpi_hpet_address(void);
uint64_t firmware_acpi_tpm2(void);
const char *firmware_bootargs(void);

#endif
