#include "boot/framebuffer.h"
#include "core/string.h"
#include "platform.h"
extern uint64_t x86_boot_framebuffer_pa;
int x86_boot_framebuffer_get(boot_framebuffer_t *out) {
    memset(out, 0, sizeof(*out));
    uint64_t pa = x86_boot_framebuffer_pa;
    if (!pa)
        return 0;
    if (pa >= 0x100000000ULL || sizeof(*out) > 0x100000000ULL - pa)
        return -1;
    memcpy(out, (const void *)(uintptr_t)(PAGE_OFFSET + pa), sizeof(*out));
    return boot_framebuffer_validate(out);
}
