#include "core/types.h"
#include "platform.h"

/* Inspect the actual shared boot translation, including board-specific block
 * splits. A normal/cacheable RAM alias is never returned for a device. */
static uint64_t device_leaf(uintptr_t va, uint64_t *span) {
    uint64_t *table = boot_pgdir;
    for (unsigned level = 0; level < 4; ++level) {
        unsigned shift = 39 - 9 * level;
        uint64_t descriptor = table[(va >> shift) & 511];
        if (!(descriptor & 1))
            return 0;
        if (level == 3 || !(descriptor & 2)) {
            if (!level || (descriptor & 0x1c))
                return 0; /* MAIR[0]: Device. */
            *span = 1ULL << shift;
            return descriptor;
        }
        table = (uint64_t *)(uintptr_t)((descriptor & 0x0000fffffffff000ULL) +
                                        PAGE_OFFSET);
    }
    return 0;
}
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa >= 0x100000000ULL || length > 0x100000000ULL - pa)
        return NULL;
    uint64_t cursor = pa, end = pa + length;
    while (cursor < end) {
        uint64_t span,
            d = device_leaf((uintptr_t)(cursor + PAGE_OFFSET), &span);
        if (!d ||
            (d & 0x0000fffffffff000ULL & ~(span - 1)) != (cursor & ~(span - 1)))
            return NULL;
        uint64_t next = (cursor & ~(span - 1)) + span;
        cursor = next < end ? next : end;
    }
    return (void *)(uintptr_t)(pa + PAGE_OFFSET);
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
