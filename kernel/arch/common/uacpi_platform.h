/* Shared implementation of the architecture OSL's opaque PCI/SCI handles.
 * This stays under arch/: address mapping and CPU IO are arch contracts. */
#include "core/types.h"
#include <uacpi/kernel_api.h>
#include "core/arch.h"
#include "firmware/acpi.h"
#include "firmware/irq.h"
#include "drivers/core/driver_hwapi.h"
#include "mm/mm.h"

extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
extern void arch_device_unmap_mmio(void *, size_t);
extern void acpi_os_interrupt_enter(void);
extern void acpi_os_interrupt_leave(void);

typedef struct {
    uacpi_pci_address address;
    volatile uint8_t *ecam;
    unsigned length;
} arch_acpi_pci_t;

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out) {
    if (!out || address.device > 31 || address.function > 7)
        return UACPI_STATUS_INVALID_ARGUMENT;
    arch_acpi_pci_t *p = kmalloc(sizeof(*p));
    if (!p)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *p = (arch_acpi_pci_t){.address = address};
    const acpi_context_t *context = arch_acpi_context();
    acpi_table_t mcfg;
    int table_status =
        context ? acpi_table_find(context, "MCFG", 0, &mcfg) : ACPI_NOT_FOUND;
    if (table_status == ACPI_OK) {
        size_t count;
        if (acpi_mcfg_count(&mcfg, &count)) {
            acpi_table_release(context, &mcfg);
            kfree(p);
            return UACPI_STATUS_INVALID_ARGUMENT;
        }
        for (size_t i = 0; i < count; ++i) {
            acpi_mcfg_entry_t e;
            if (acpi_mcfg_get(&mcfg, i, &e) || e.segment != address.segment ||
                address.bus < e.start_bus || address.bus > e.end_bus)
                continue;
            uint64_t offset = ((uint64_t)address.bus << 20) |
                              ((uint64_t)address.device << 15) |
                              ((uint64_t)address.function << 12);
            if (e.base <= (~(uint64_t)0) - offset)
                p->ecam = arch_device_map_mmio(e.base + offset, 4096, 0);
            p->length = p->ecam ? 4096 : 0;
            break;
        }
        acpi_table_release(context, &mcfg);
        /* An advertised MCFG is authoritative; never silently use CF8 for a
         * segment/bus missing from it or for an inaccessible mapping. */
        if (!p->length) {
            kfree(p);
            return UACPI_STATUS_NOT_FOUND;
        }
    } else if (table_status != ACPI_NOT_FOUND) {
        kfree(p);
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    } else if (!address.segment && ARCH_ACPI_LEGACY_PCI) {
        p->length = 256;
    } else {
        kfree(p);
        return UACPI_STATUS_NOT_FOUND;
    }
    *out = p;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    arch_acpi_pci_t *p = handle;
    if (!p)
        return;
    if (p->ecam)
        arch_device_unmap_mmio((void *)p->ecam, 4096);
    kfree(p);
}
static uacpi_status arch_acpi_pci_access(uacpi_handle handle, uacpi_size offset,
                                         unsigned width, int write,
                                         uint32_t *value) {
    arch_acpi_pci_t *p = handle;
    if (!p || !value || offset >= p->length || width > p->length - offset ||
        (offset & (width - 1)))
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (!p->ecam)
        return arch_acpi_legacy_pci_access(p->address, offset, width, write,
                                           value);
    volatile void *ptr = p->ecam + offset;
    if (write) {
        if (width == 1)
            *(volatile uint8_t *)ptr = (uint8_t)*value;
        else if (width == 2)
            *(volatile uint16_t *)ptr = (uint16_t)*value;
        else
            *(volatile uint32_t *)ptr = *value;
        wmb();
    } else {
        if (width == 1)
            *value = *(volatile uint8_t *)ptr;
        else if (width == 2)
            *value = *(volatile uint16_t *)ptr;
        else
            *value = *(volatile uint32_t *)ptr;
        rmb();
    }
    return UACPI_STATUS_OK;
}
#define ACPI_PCI_ACCESSORS(bits)                                               \
    uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size o,     \
                                             uacpi_u##bits *v) {               \
        if (!v)                                                                \
            return UACPI_STATUS_INVALID_ARGUMENT;                              \
        uint32_t n = 0;                                                        \
        uacpi_status s = arch_acpi_pci_access(h, o, bits / 8, 0, &n);          \
        if (s == UACPI_STATUS_OK) {                                            \
            *v = (uacpi_u##bits)n;                                             \
        }                                                                      \
        return s;                                                              \
    }                                                                          \
    uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size o,    \
                                              uacpi_u##bits v) {               \
        uint32_t n = v;                                                        \
        return arch_acpi_pci_access(h, o, bits / 8, 1, &n);                    \
    }
ACPI_PCI_ACCESSORS(8)
ACPI_PCI_ACCESSORS(16)
ACPI_PCI_ACCESSORS(32)
#undef ACPI_PCI_ACCESSORS

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t state = arch_read_sstatus();
    arch_local_irq_disable();
    return state;
}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    arch_write_sstatus(state);
}

typedef struct {
    uacpi_interrupt_handler handler;
    uacpi_handle context;
    uint32_t irq;
} arch_acpi_irq_t;
static int arch_acpi_sci(int irq, void *opaque) {
    (void)irq;
    arch_acpi_irq_t *sci = opaque;
    acpi_os_interrupt_enter();
    uacpi_interrupt_ret result = sci->handler(sci->context);
    acpi_os_interrupt_leave();
    return result == UACPI_INTERRUPT_HANDLED;
}
uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context,
    uacpi_handle *out) {
    if (!handler || !out)
        return UACPI_STATUS_INVALID_ARGUMENT;
    int mapped_irq = arch_firmware_irq_map(irq, ARCH_ACPI_SCI_FLAGS);
    if (mapped_irq < 0)
        return UACPI_STATUS_UNIMPLEMENTED;
    arch_acpi_irq_t *sci = kmalloc(sizeof(*sci));
    if (!sci)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *sci = (arch_acpi_irq_t){
        .handler = handler, .context = context, .irq = (uint32_t)mapped_irq};
    if (request_irq((uint32_t)mapped_irq, arch_acpi_sci, IRQF_SHARED, sci) <
        0) {
        kfree(sci);
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out = sci;
    return UACPI_STATUS_OK;
}
uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                         uacpi_handle handle) {
    arch_acpi_irq_t *sci = handle;
    if (!sci || sci->handler != handler)
        return UACPI_STATUS_INVALID_ARGUMENT;
    free_irq(sci->irq, sci);
    kfree(sci);
    return UACPI_STATUS_OK;
}
