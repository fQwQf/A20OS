#include "core/types.h"
#include "platform.h"

/* DMW1/DMW0 provide the uncached VSEG8 alias installed by entry.S. */
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa >= 0x100000000ULL || length > 0x100000000ULL - pa)
        return NULL;
    return (void *)(uintptr_t)(pa | 0x8000000000000000ULL);
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
