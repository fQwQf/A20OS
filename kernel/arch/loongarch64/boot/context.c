#include "core/arch.h"
#include "../../common/boot_context.h"
extern uint64_t __boot_dtb_ptr;
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
    uint64_t raw = __boot_dtb_ptr;
    if (raw) {
        /* VSEG 8/9 are uncached/cached aliases, never CPU physical addresses. */
        uint64_t pa = raw & 0x0fffffffffffffffULL;
        uint64_t base = PHYS_MEMORY_BASE, end = PHYS_MEMORY_END;
        if (pa < 0x10000000ULL && PAGE_OFFSET == 0) {
            base = 0;
            end = 0x10000000ULL; /* QEMU's low RAM DMW bank. */
        }
        context.protocol = BOOT_PROTOCOL_FDT;
        context.fdt_available = arch_boot_mapped_span(pa, base, end);
        context.fdt_physical = pa;
        context.fdt = (const void *)(uintptr_t)(pa + PAGE_OFFSET);
    }
    arch_boot_publish(&context);
}
