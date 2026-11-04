/*
 * MCU identity mapping for platform-device MMIO resources.
 *
 * The RV32 M-mode profile is NOMMU: a physical peripheral address is directly
 * usable by the CPU.  Drivers still obtain it through the standard
 * device_map_mmio() path, so no driver includes an arch header.
 */
#include "core/types.h"

void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags)
{
    (void)flags;
    if (!length || pa != (uint64_t)(uintptr_t)pa)
        return NULL;
    return (void *)(uintptr_t)pa;
}

void arch_device_unmap_mmio(void *mapped, size_t length)
{
    (void)mapped;
    (void)length;
}
