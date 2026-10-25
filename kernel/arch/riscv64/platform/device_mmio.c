#include "core/types.h"
#include "platform.h"

#include "drivers/core/driver_core.h"
extern uint64_t boot_pgdir[];
/* Standard RISC-V PMAs define the device access semantics; Sv39 entries do
 * not add a cacheable alias. Verify every leaf against the live boot map. */
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    uint64_t ram = current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
    if (!length || pa >= ram || length > ram - pa)
        return NULL;
#ifdef CONFIG_NOMMU
    return (void *)(uintptr_t)pa;
#else
    uint64_t end = pa + length;
    for (uint64_t cursor = pa; cursor < end;) {
        uint64_t *table = boot_pgdir, span = 0, d = 0;
        uintptr_t va = (uintptr_t)(cursor + PAGE_OFFSET);
        for (int level = 2; level >= 0; --level) {
            unsigned shift = 12 + level * 9;
            d = table[(va >> shift) & 511];
            if (!(d & 1))
                return NULL;
            if (d & 0xe) {
                span = 1ULL << shift;
                break;
            }
            table = (uint64_t *)(uintptr_t)(((d >> 10) << 12) + PAGE_OFFSET);
        }
        if (!span || !(d & 2) || !(d & 4) ||
            ((((d >> 10) << 12) & ~(span - 1)) != (cursor & ~(span - 1))))
            return NULL;
        cursor = (cursor & ~(span - 1)) + span;
    }
    return (void *)(uintptr_t)(pa + PAGE_OFFSET);
#endif
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
