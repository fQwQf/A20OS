#include "core/types.h"
#include "platform.h"

void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa < 0x1c000000ULL || pa >= 0x20000000ULL ||
        length > 0x20000000ULL - pa)
        return NULL;
    return (void *)(uintptr_t)pa; /* Uncached DMW window installed at entry. */
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
