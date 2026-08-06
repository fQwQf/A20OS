/*
 * virtio-mmio transport — shared register protocol (dual-placement).
 *
 * Register-level virtio-mmio (v1/v2) transport operations used by
 * dual-placement virtio drivers.  Probe and feature/config reads are
 * non-destructive; queue setup and status transitions are destructive
 * and therefore owned by exactly one placement at a time (device
 * ownership arbitration is still open, see 04-dual-placement.md).
 */
#ifndef _DRIVERS_DUAL_VIRTIO_MMIO_H
#define _DRIVERS_DUAL_VIRTIO_MMIO_H

#include "drivers/dual/drv_env.h"

#define VMMIO_MAGIC             0x000u  /* "virt" = 0x74726976 */
#define VMMIO_VERSION           0x004u
#define VMMIO_DEVICE_ID         0x008u
#define VMMIO_VENDOR_ID         0x00cu
#define VMMIO_DEV_FEATURES      0x010u
#define VMMIO_DEV_FEATURES_SEL  0x014u
#define VMMIO_DRV_FEATURES      0x020u
#define VMMIO_DRV_FEATURES_SEL  0x024u
#define VMMIO_QUEUE_SEL         0x030u
#define VMMIO_QUEUE_NUM_MAX     0x034u
#define VMMIO_QUEUE_NUM         0x038u
#define VMMIO_QUEUE_PFN         0x040u  /* legacy only */
#define VMMIO_QUEUE_READY       0x044u
#define VMMIO_QUEUE_NOTIFY      0x050u
#define VMMIO_INTR_STATUS       0x060u
#define VMMIO_INTR_ACK          0x064u
#define VMMIO_STATUS            0x070u
#define VMMIO_CONFIG_GEN        0x0fcu
#define VMMIO_CONFIG            0x100u  /* device config space base */

#define VMMIO_MAGIC_VALUE       0x74726976u

#define VIRTIO_STATUS_ACK       1u
#define VIRTIO_STATUS_DRIVER    2u
#define VIRTIO_STATUS_FEATURES_OK 8u
#define VIRTIO_STATUS_DRIVER_OK 4u

typedef struct vmmio_probe {
    uint32_t magic;
    uint32_t version;
    uint32_t device_id;
    uint32_t vendor_id;
} vmmio_probe_t;

static inline int vmmio_probe(uint64_t base, vmmio_probe_t *out)
{
    vmmio_probe_t p;
    p.magic     = drv_mmio_read32(base, VMMIO_MAGIC);
    p.version   = drv_mmio_read32(base, VMMIO_VERSION);
    p.device_id = drv_mmio_read32(base, VMMIO_DEVICE_ID);
    p.vendor_id = drv_mmio_read32(base, VMMIO_VENDOR_ID);
    if (p.magic != VMMIO_MAGIC_VALUE)
        return -1;
    if (p.version != 1 && p.version != 2)
        return -1;
    if (p.device_id == 0)
        return -1;
    if (out)
        *out = p;
    return 0;
}

static inline uint32_t vmmio_dev_feature(uint64_t base, uint32_t sel)
{
    drv_mmio_write32(base, VMMIO_DEV_FEATURES_SEL, sel);
    return drv_mmio_read32(base, VMMIO_DEV_FEATURES);
}

static inline uint8_t vmmio_cfg_read8(uint64_t base, uint32_t off)
{
    return drv_mmio_read8(base, VMMIO_CONFIG + off);
}

static inline void vmmio_cfg_write8(uint64_t base, uint32_t off, uint8_t v)
{
    drv_mmio_write8(base, VMMIO_CONFIG + off, v);
}

#endif
