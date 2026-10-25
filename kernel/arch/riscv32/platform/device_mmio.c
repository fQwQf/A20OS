#include "core/types.h"
#include "platform.h"

void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa >= 0x40000000ULL || length > 0x40000000ULL - pa)
        return NULL;
#ifdef CONFIG_NOMMU
    return (void *)(uintptr_t)pa;
#else
    uint64_t end = pa + length;
    for (uint64_t cursor = pa; cursor < end;) {
        uint32_t va = (uint32_t)cursor + 0xc0000000U;
        uint32_t d = boot_pgdir[va >> 22];
        if ((d & 7) != 7 || (((uint64_t)(d >> 10) << 12) & ~0x3fffffULL) !=
                                (cursor & ~0x3fffffULL))
            return NULL;
        cursor = (cursor & ~0x3fffffULL) + 0x400000;
    }
    return (void *)(uintptr_t)((uint32_t)pa + 0xc0000000U);
#endif
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
