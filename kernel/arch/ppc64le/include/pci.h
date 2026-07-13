#ifndef _ARCH_PPC64LE_PCI_H
#define _ARCH_PPC64LE_PCI_H

#include "core/types.h"

#define PCI_VENDOR_ID_REDHAT    0x1AF4

#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      (1U << 1)
#define PCI_COMMAND_BUS_MASTER  (1U << 2)

#define PCI_STATUS              0x06
#define PCI_STATUS_CAP_LIST     (1U << 4)

#define PCI_BAR0                0x10
#define PCI_CAPABILITIES_PTR    0x34
#define PCI_CAP_ID_VNDR         0x09

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

#define PCI_MAX_DEV             32

typedef struct {
    int       valid;
    int       dev_num;
    int       device_type;
    uintptr_t common_base;
    uintptr_t notify_base;
    uintptr_t config_base;
    uintptr_t isr_base;
    uint32_t  notify_off_multiplier;
} pci_virtio_dev_t;

#endif
