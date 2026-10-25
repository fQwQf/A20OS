#include "core/arch.h"
#include "core/bootargs.h"
#include "boot/context.h"
#include "core/panic.h"
extern uint64_t x86_booted_uefi;
extern int x86_boot_framebuffer_get(boot_framebuffer_t *);
extern uint64_t arch_firmware_rsdp_pa(void);
void arch_boot_context_init(void) {
    boot_context_t context = {
        .protocol =
            x86_booted_uefi ? BOOT_PROTOCOL_UEFI : BOOT_PROTOCOL_MULTIBOOT,
        .acpi_rsdp = arch_firmware_rsdp_pa(),
        .command_line = arch_bootargs_get(),
    };
    if (x86_boot_framebuffer_get(&context.framebuffer) < 0)
        panic("boot: invalid framebuffer handoff");
    if (boot_context_publish(&context) < 0)
        panic("boot: invalid x86 firmware handoff");
}

#ifdef CONFIG_ACPI
#include "firmware/acpi.h"
extern const acpi_context_t *firmware_acpi_context(void);
const acpi_context_t *arch_acpi_context(void) {
    return firmware_acpi_context();
}
#endif
