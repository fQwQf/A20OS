#ifdef CONFIG_PPC64LE
#include "core/bootargs.h"
#include "boot/context.h"
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
}
#endif
