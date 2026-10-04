#ifndef _ARCH_X86_64_PCI_H
#define _ARCH_X86_64_PCI_H

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

/* QueueNotifyOff is fixed for the device's lifetime, but reading it back costs
 * an MMIO round trip on every kick, and the kick is the one register write a
 * virtio driver performs per buffer.  Cached per queue index; see the notify
 * case in platform/virtio_probe.c.  A queue index outside the cache falls back
 * to the read, so a wide device is slower rather than wrong. */
#define PCI_VIRTIO_NOTIFY_CACHE 8

typedef struct {
    int      valid;
    int      dev_num;
    int      device_type;
    int      irq;
    uintptr_t common_base;
    uintptr_t notify_base;
    uintptr_t config_base;
    uint32_t  notify_off_multiplier;
    uintptr_t isr_base;
    /* Bit i set: notify_off_cache[i] holds queue i's QueueNotifyOff. */
    uint32_t  notify_off_cached;
    uint16_t  notify_off_cache[PCI_VIRTIO_NOTIFY_CACHE];
} pci_virtio_dev_t;

void pci_init(void);

#endif
