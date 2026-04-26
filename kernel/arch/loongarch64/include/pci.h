#ifndef _ARCH_LOONGARCH64_PCI_H
#define _ARCH_LOONGARCH64_PCI_H

#include "core/types.h"

#define PCI_VENDOR_ID_REDHAT    0x1AF4
#define PCI_DEVICE_ID_VIRTIO_10 0x1040

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      (1 << 1)
#define PCI_COMMAND_BUS_MASTER  (1 << 2)

#define PCI_STATUS              0x06
#define PCI_STATUS_CAP_LIST     (1 << 4)

#define PCI_HEADER_TYPE         0x0E
#define PCI_HEADER_TYPE_MULTI   0x80

#define PCI_BAR0                0x10
#define PCI_CAPABILITIES_PTR    0x34
#define PCI_INTERRUPT_LINE      0x3C

#define PCI_CAP_ID_VNDR         0x09

#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

#define PCI_MAX_DEV             32

typedef struct {
    int      valid;
    int      dev_num;
    uintptr_t common_base;
    uintptr_t notify_base;
    uintptr_t config_base;
    uint32_t  notify_off_multiplier;
    uintptr_t isr_base;
} pci_virtio_dev_t;

void pci_init(void);

#endif
