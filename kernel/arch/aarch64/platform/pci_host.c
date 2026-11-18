#include "drivers/bus/pci_hal.h"
#include "platform.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
static uintptr_t pci_ecam_base;
void arch_pci_host_init(uintptr_t ecam_base) { pci_ecam_base = ecam_base; }
static volatile uint32_t *pci_config(int bus,int dev,int func,uint32_t reg) {
    if (!pci_ecam_base || bus < 0 || bus > 255 ||
        dev < 0 || dev > 31 || func < 0 || func > 7 || reg > 4092 || (reg & 3)) return NULL;
    uint64_t offset = ((uint64_t)bus << 20) | ((uint64_t)dev << 15) | ((uint64_t)func << 12) | reg;
    uint64_t base;
    if (__builtin_sub_overflow((uint64_t)pci_ecam_base,
                              (uint64_t)PAGE_OFFSET, &base)) return NULL;
    if (base > (~(uint64_t)0) - offset) return NULL;
    return arch_device_map_mmio(base + offset, 4, 0);
}
uint32_t arch_pci_config_read32(int bus,int dev,int func,uint32_t reg) {
    volatile uint32_t *p = pci_config(bus,dev,func,reg);
    return p ? *p : 0xffffffffU;
}
void arch_pci_config_write32(int bus,int dev,int func,uint32_t reg,uint32_t value) {
    volatile uint32_t *p = pci_config(bus,dev,func,reg);
    if (p) *p = value;
}
uintptr_t arch_pci_bar_to_resource(uint64_t pa) {
    return (uintptr_t)arch_device_map_mmio(pa, 1, 0);
}

#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
int arch_pci_mmio_window(uint64_t *base, uint64_t *size) {
    if (!base || !size) return -EINVAL;
    *base = 0; *size = 0;
    return -EOPNOTSUPP;
}
int arch_pci_bar_reachable(uint64_t base, uint64_t size) {
    if (!size || size > (uint64_t)(~(size_t)0)) return 0;
    return arch_device_map_mmio(base, (size_t)size, 0) != NULL;
}
