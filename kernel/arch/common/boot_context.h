#ifndef A20_ARCH_BOOT_CONTEXT_H
#define A20_ARCH_BOOT_CONTEXT_H
#include "boot/context.h"
#include "firmware/devicetree.h"
#include "core/panic.h"
#include "core/errno.h"

/* Architectures supply the address translation and independently mapped span.
 * The description and node context stay alive for every device handle. */
static inline void arch_boot_publish(boot_context_t *context) {
    static fw_dt_t tree;
    static char command_line[1024];
    if (context->fdt) {
        if (fw_dt_init(&tree, context->fdt, context->fdt_available) < 0 ||
            fw_dt_root(&tree, &context->hardware_root) < 0)
            panic("boot: invalid firmware device tree");
        if (fw_dt_framebuffer_select(&tree, &context->framebuffer,
                                      &context->framebuffer_memory,
                                      &context->framebuffer_node) < 0)
            panic("boot: invalid or ambiguous DT framebuffer handoff");
        if (context->framebuffer.magic &&
            arch_boot_framebuffer_prepare(&context->framebuffer,
                &context->framebuffer_memory, context->fdt_physical,
                tree.view.size) < 0)
            panic("boot: unsupported framebuffer memory mapping");
        int result = fw_dt_bootargs(&tree, command_line, sizeof(command_line));
        if (result < 0 && result != -ENOENT)
            panic("boot: invalid or oversized firmware command line");
        if (result == -ENOENT)
            command_line[0] = 0;
        context->command_line = command_line;
    }
    if (boot_context_publish(context) < 0)
        panic("boot: duplicate or invalid architecture handoff");
}
static inline size_t arch_boot_mapped_span(uint64_t physical, uint64_t base,
                                           uint64_t end) {
    if ((physical & 7) || physical < base || physical >= end)
        panic("boot: firmware DTB outside architecture RAM mapping");
    uint64_t span = end - physical;
    return span < 0x200000 ? (size_t)span : 0x200000;
}
#endif
