#include "core/types.h"
#include "platform.h"

void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa + length < pa)
        return NULL;
    if ((pa >= 0x40000000 && pa + length <= 0x60000000) ||
        (pa >= 0xe0000000 && pa + length <= 0xe0100000))
        return (void *)(uintptr_t)pa;
    return NULL;
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
