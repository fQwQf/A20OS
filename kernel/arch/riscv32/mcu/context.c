#include "boot/context.h"
#include "core/panic.h"
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
    if (boot_context_publish(&context) < 0)
        panic("RV32 MCU: invalid boot context");
}
