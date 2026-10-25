#include "boot/context.h"
#include "core/panic.h"
/* This architecture's existing entry publishes no firmware-description ABI. */
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
    if (boot_context_publish(&context) < 0)
        panic("boot: invalid static architecture handoff");
}
