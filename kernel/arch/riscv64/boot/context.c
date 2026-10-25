#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "../../common/boot_context.h"
extern uint64_t __boot_dtb_ptr;
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
    uint64_t pa = __boot_dtb_ptr;
    if (pa) {
        uint64_t base =
            current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
        uint64_t end = current_board ? current_board->ram_end : PHYS_MEMORY_END;
#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
        end =
            PHYS_MEMORY_MAX_END; /* Entry maps the full dynamic-QEMU RAM ceiling. */
#endif
        context.protocol = BOOT_PROTOCOL_FDT;
        context.fdt_available = arch_boot_mapped_span(pa, base, end);
        context.fdt_physical = pa;
        context.fdt = (const void *)(uintptr_t)(pa + PAGE_OFFSET);
    }
    arch_boot_publish(&context);
}
