#include "core/arch.h"
#include "../../common/boot_context.h"
extern uint32_t arm32_boot_dtb; /* physical .text.entry word */
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
    uint64_t pa = *(const uint32_t *)((uintptr_t)&arm32_boot_dtb + PAGE_OFFSET);
    if (pa) {
        context.protocol = BOOT_PROTOCOL_FDT;
        context.fdt_available =
            arch_boot_mapped_span(pa, PHYS_MEMORY_BASE, PHYS_MEMORY_END);
        context.fdt_physical = pa;
        context.fdt = (const void *)(uintptr_t)(pa + PAGE_OFFSET);
    }
    arch_boot_publish(&context);
}
