#include "core/types.h"
#include <uacpi/kernel_api.h>
#include "cpu.h"
#include "drivers/bus/pci_hal.h"
#define ARCH_ACPI_LEGACY_PCI 1
#define ARCH_ACPI_SCI_FLAGS 8
extern int x86_pci_legacy_access(int, int, int, unsigned, unsigned, int,
                                 uint32_t *);
static uacpi_status arch_acpi_legacy_pci_access(uacpi_pci_address p,
                                                uacpi_size offset,
                                                unsigned width, int write,
                                                uint32_t *value) {
    return x86_pci_legacy_access(p.bus, p.device, p.function, offset, width,
                                 write, value)
               ? UACPI_STATUS_INVALID_ARGUMENT
               : UACPI_STATUS_OK;
}
#include "../../common/uacpi_platform.h"
unsigned arch_acpi_interrupt_model(void) {
    return 1;
} /* IOAPIC */
typedef struct {
    uint32_t base, length;
} arch_acpi_io_t;
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length,
                                 uacpi_handle *out) {
    if (!out || !length || base >= 65536 || length > 65536 - base)
        return UACPI_STATUS_INVALID_ARGUMENT;
    arch_acpi_io_t *p = kmalloc(sizeof(*p));
    if (!p)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *p = (arch_acpi_io_t){.base = base, .length = length};
    *out = p;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {
    kfree(h);
}
#define ACPI_IO_ACCESSORS(bits, suffix)                                        \
    uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size o,      \
                                            uacpi_u##bits *v) {                \
        arch_acpi_io_t *p = h;                                                 \
        if (!p || !v || o >= p->length || bits / 8 > p->length - o)            \
            return UACPI_STATUS_INVALID_ARGUMENT;                              \
        *v = in##suffix((uint16_t)(p->base + o));                              \
        return UACPI_STATUS_OK;                                                \
    }                                                                          \
    uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size o,     \
                                             uacpi_u##bits v) {                \
        arch_acpi_io_t *p = h;                                                 \
        if (!p || o >= p->length || bits / 8 > p->length - o)                  \
            return UACPI_STATUS_INVALID_ARGUMENT;                              \
        out##suffix((uint16_t)(p->base + o), v);                               \
        return UACPI_STATUS_OK;                                                \
    }
ACPI_IO_ACCESSORS(8, b)
ACPI_IO_ACCESSORS(16, w)
ACPI_IO_ACCESSORS(32, l)
