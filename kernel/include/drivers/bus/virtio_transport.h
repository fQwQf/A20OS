#ifndef _VIRTIO_TRANSPORT_H
#define _VIRTIO_TRANSPORT_H

#include "core/types.h"

/* ============================================================
 * VirtIO Transport Abstraction
 *
 * Decouples the virtio-blk driver from the underlying transport
 * (MMIO on RISC-V, PCI modern on LoongArch).  The driver calls
 * read32/write32 through function pointers; the arch layer
 * provides the implementation and device discovery.
 *
 * Register offsets passed to read32/write32 are the MMIO-style
 * offsets (VIRTIO_MMIO_* defines from virtio_blk.h).  The
 * transport internally translates them to the correct BAR +
 * offset for PCI, or does a plain memory access for MMIO.
 * ============================================================ */

typedef struct virtio_transport {
    /* Read / write a 32-bit register (MMIO-style offset). */
    uint32_t (*read32)(struct virtio_transport *t, uint32_t off);
    void     (*write32)(struct virtio_transport *t, uint32_t off, uint32_t val);

    /* Transport-private data (e.g. MMIO base or PCI BAR info). */
    void    *priv;

    /* Set by arch probe: 1 = legacy virtio (PFN-based queue setup). */
    int      legacy;

    /* Platform IRQ line for this device, or -1 if not IRQ-driven. */
    int      irq;

    /* 1 when the IRQ is a shareable PCI INTx line: the driver must pass
     * IRQF_SHARED at registration so several functions on one GSI can
     * coexist, and must keep device-level interrupts disabled when the
     * registration failed (a source without a handler would hold the
     * shared level line asserted forever). */
    int      shared_irq;
} virtio_transport_t;

/* Probe for the next virtio-blk device.
 * index = sequential device number (0, 1, 2 …).
 * On success, fills *vt and returns 0.
 * On failure (no more devices), returns -1. */
int arch_virtio_blk_probe(int index, virtio_transport_t *vt);
int arch_virtio_net_probe(int index, virtio_transport_t *vt);

#endif /* _VIRTIO_TRANSPORT_H */
