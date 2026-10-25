#include "core/types.h"
#include "platform.h"

/* pseries uses logical CI/RTAS operations rather than a directly addressable
 * physical MMIO alias. A driver needing MMIO must use that transport. */
void *arch_device_map_mmio(uint64_t pa, size_t length, uint32_t flags) {
    (void)pa;
    (void)length;
    (void)flags;
    return NULL;
}

void arch_device_unmap_mmio(void *mapped, size_t length) {
    (void)mapped;
    (void)length; /* Permanent, shared boot mapping. */
}
