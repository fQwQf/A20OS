/* Compiled with A20OS headers so the private BSD image cannot bake in a
 * foreign kernel's direct-map offset. The allocator returns mapped backing. */
#include "core/arch.h"
#include "core/consts.h"


uintptr_t port_phys_to_dmap(uint64_t address)
{
    return (uintptr_t)(address + PAGE_OFFSET);
}

uint64_t port_dmap_to_phys(uintptr_t address)
{
    return (uint64_t)address - PAGE_OFFSET;
}
