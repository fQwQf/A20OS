#include "core/types.h"
#include "platform.h"

extern uint32_t boot_pgdir[];
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)flags;
    if (!length || pa >= 0x80000000ULL || length > 0x80000000ULL - pa)
        return NULL;
#ifdef CONFIG_NOMMU
    /* The MMU is disabled; architected default Device attributes apply. */
    return (void *)(uintptr_t)pa;
#else
    uint64_t end = pa + length;
    for (uint64_t cursor = pa; cursor < end;) {
        uint32_t d = boot_pgdir[(uint32_t)(cursor + PAGE_OFFSET) >> 20];
        /* Short-descriptor section, TEX=0, C=0, B=1 (shared Device). */
        if ((d & 3) != 2 || (d & 0x700c) != 4 ||
            (d & 0xfff00000U) != (cursor & 0xfff00000ULL))
            return NULL;
        cursor = (cursor & ~0xfffffULL) + 0x100000;
    }
    return (void *)(uintptr_t)(pa + PAGE_OFFSET);
#endif
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
