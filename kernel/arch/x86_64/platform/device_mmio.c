#include "core/types.h"
#include "platform.h"

#include "cpu.h"
/* MMIO must resolve through an existing UC leaf. Never expose the boot map's
 * WB aliases below 2 GiB as if they were safe device mappings. */
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa >= 0x100000000ULL || length > 0x100000000ULL - pa)
        return NULL;
    uint64_t pat = rdmsr(0x277), cursor = pa, end = pa + length;
    while (cursor < end) {
        uint64_t *table = boot_pgdir, d = 0, span = 0;
        uintptr_t va = (uintptr_t)(cursor + PAGE_OFFSET);
        for (unsigned level = 0; level < 4; ++level) {
            unsigned shift = 39 - level * 9;
            d = table[(va >> shift) & 511];
            if (!(d & 1))
                return NULL;
            if (level == 3 || (level && (d & 0x80))) {
                span = 1ULL << shift;
                unsigned index =
                    (unsigned)((d >> 3) & 3) |
                    (unsigned)(((d >> (level == 3 ? 7 : 12)) & 1) << 2);
                if (((pat >> (index * 8)) & 0xff) != 0)
                    return NULL;
                break;
            }
            table = (uint64_t *)(uintptr_t)((d & 0x000ffffffffff000ULL) +
                                            PAGE_OFFSET);
        }
        if (!span ||
            (d & 0x000ffffffffff000ULL & ~(span - 1)) != (cursor & ~(span - 1)))
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
