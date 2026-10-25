#include "core/types.h"
#include <uacpi/kernel_api.h>
#define ARCH_ACPI_LEGACY_PCI 0
#define ARCH_ACPI_SCI_FLAGS 4
static uacpi_status arch_acpi_legacy_pci_access(uacpi_pci_address p,
                                                uacpi_size o, unsigned w,
                                                int wr, uint32_t *v) {
    (void)p;
    (void)o;
    (void)w;
    (void)wr;
    (void)v;
    return UACPI_STATUS_UNIMPLEMENTED;
}
#include "../../common/uacpi_platform.h"
unsigned arch_acpi_interrupt_model(void) {
    return 4;
} /* GIC */
/* ARM's ACPI SystemIO space has no implicit x86 port-I/O mechanism. */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length,
                                 uacpi_handle *out) {
    (void)base;
    (void)length;
    if (out)
        *out = NULL;
    return UACPI_STATUS_UNIMPLEMENTED;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {
    (void)h;
}
#define ACPI_NO_IO(bits)                                                       \
    uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size o,      \
                                            uacpi_u##bits *v) {                \
        (void)h;                                                               \
        (void)o;                                                               \
        (void)v;                                                               \
        return UACPI_STATUS_UNIMPLEMENTED;                                     \
    }                                                                          \
    uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size o,     \
                                             uacpi_u##bits v) {                \
        (void)h;                                                               \
        (void)o;                                                               \
        (void)v;                                                               \
        return UACPI_STATUS_UNIMPLEMENTED;                                     \
    }
ACPI_NO_IO(8)
ACPI_NO_IO(16)
ACPI_NO_IO(32)

#include "core/errno.h"
int arch_firmware_irq_map(uint32_t hardware, uint32_t flags) {
    if (hardware < 32 || hardware >= 1020)
        return -EINVAL;
    if (flags != 1 && flags != 4)
        return -EOPNOTSUPP;
    uint32_t lines =
        ((*(volatile uint32_t *)(uintptr_t)(GICD_BASE + 4) & 31U) + 1U) * 32U;
    if (hardware >= lines)
        return -EOPNOTSUPP;
    volatile uint32_t *icfgr =
        (volatile uint32_t *)(uintptr_t)(GICD_BASE + 0xc00 +
                                         (hardware / 16) * 4);
    uint32_t shift = (hardware % 16) * 2;
    uint32_t config = *icfgr & ~(2U << shift);
    if (flags == 1)
        config |= 2U << shift;
    *icfgr = config;
    mb();
    return (int)hardware;
}
