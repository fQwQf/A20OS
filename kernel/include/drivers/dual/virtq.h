/*
 * virtq — shared split-virtqueue layer (dual-placement).
 *
 * Minimal split virtqueue over a drv_dma buffer for modern (v2) mmio
 * devices.  Single-owner by design: queue setup is destructive, so only
 * the placement that owns the device may call virtq_init (see
 * 04-dual-placement.md ownership rules).  All rings live inside one DMA
 * page (num <= 8), matching the drv_dma user-placement contiguity limit.
 */
#ifndef _DRIVERS_DUAL_VIRTQ_H
#define _DRIVERS_DUAL_VIRTQ_H

#include "drivers/dual/virtio_mmio.h"

#define VIRTQ_DESC_F_NEXT  1u
#define VIRTQ_DESC_F_WRITE 2u

#define VIRTQ_MAX_NUM 8u

typedef struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_NUM];
    uint16_t used_event;
} virtq_avail_t;

typedef struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTQ_MAX_NUM];
    uint16_t avail_event;
} virtq_used_t;

typedef struct virtq {
    uint32_t qsel;
    uint32_t num;
    uint64_t mmio;              /* device mmio base (CPU VA) */
    uint64_t va;                /* ring area VA */
    uint64_t desc_pa, avail_pa, used_pa;
    uint32_t buf_off;           /* first byte past the rings (data area) */
    uint16_t last_used;
} virtq_t;

static inline volatile virtq_desc_t *virtq_descs(const virtq_t *q)
{
    return (volatile virtq_desc_t *)(uintptr_t)q->va;
}

static inline volatile virtq_avail_t *virtq_avail(const virtq_t *q)
{
    return (volatile virtq_avail_t *)(uintptr_t)(q->va + 16u * q->num);
}

static inline volatile virtq_used_t *virtq_used(const virtq_t *q)
{
    uint64_t off = 16u * q->num + 6u + 2u * q->num;
    off = (off + 3u) & ~3ull;
    return (volatile virtq_used_t *)(uintptr_t)(q->va + off);
}

/* Initialize queue @qsel with @num slots inside @dma page 0.
 * Returns 0, or -1 if the device cannot host the queue. */
static inline int virtq_init(virtq_t *q, uint64_t mmio, uint32_t qsel,
                             uint32_t num, const drv_dma_t *dma)
{
    if (!q || !dma || num == 0 || num > VIRTQ_MAX_NUM)
        return -1;
    uint64_t avail_end = 16ull * num + 6u + 2u * num;
    uint64_t used_off = (avail_end + 3u) & ~3ull;
    uint64_t used_size = 6u + 8u * num;
    if (used_off + used_size >= DRV_PAGE_SIZE)
        return -1;

    q->qsel = qsel;
    q->num = num;
    q->mmio = mmio;
    q->va = drv_dma_va(dma, 0);
    q->last_used = 0;

    drv_mmio_write32(mmio, VMMIO_QUEUE_SEL, qsel);
    uint32_t max = drv_mmio_read32(mmio, VMMIO_QUEUE_NUM_MAX);
    if (max == 0 || num > max)
        return -1;
    drv_mmio_write32(mmio, VMMIO_QUEUE_NUM, num);

    uint64_t base_pa = drv_dma_phys(dma, 0);
    if (!base_pa)
        return -1;
    q->desc_pa = base_pa;
    q->avail_pa = base_pa + 16ull * num;
    q->used_pa = base_pa + used_off;
    q->buf_off = (uint32_t)(used_off + used_size);

    drv_mmio_write32(mmio, 0x080, (uint32_t)q->desc_pa);   /* QUEUE_DESC_LO */
    drv_mmio_write32(mmio, 0x084, (uint32_t)(q->desc_pa >> 32));
    drv_mmio_write32(mmio, 0x090, (uint32_t)q->avail_pa);  /* QUEUE_AVAIL_LO */
    drv_mmio_write32(mmio, 0x094, (uint32_t)(q->avail_pa >> 32));
    drv_mmio_write32(mmio, 0x0a0, (uint32_t)q->used_pa);   /* QUEUE_DEVICE_LO */
    drv_mmio_write32(mmio, 0x0a4, (uint32_t)(q->used_pa >> 32));
    drv_mmio_write32(mmio, VMMIO_QUEUE_READY, 1);
    return 0;
}

/* Post one device-writable buffer (data area offset @slot_off within the
 * same page) as descriptor @d and make it available. */
static inline void virtq_post_inbuf(virtq_t *q, uint32_t d, uint64_t buf_pa,
                                    uint32_t len)
{
    volatile virtq_desc_t *desc = virtq_descs(q);
    desc[d].addr = buf_pa;
    desc[d].len = len;
    desc[d].flags = VIRTQ_DESC_F_WRITE;
    desc[d].next = 0;
    volatile virtq_avail_t *av = virtq_avail(q);
    uint16_t idx = av->idx;
    av->ring[idx % q->num] = (uint16_t)d;
    __sync_synchronize();
    av->idx = idx + 1;
    __sync_synchronize();
}

static inline void virtq_notify(virtq_t *q)
{
    drv_mmio_write32(q->mmio, VMMIO_QUEUE_NOTIFY, q->qsel);
}

/* Poll the used ring; returns 1 and fills @out_id/@out_len when the
 * device consumed a buffer since the last poll. */
static inline int virtq_poll_used(virtq_t *q, uint32_t *out_id, uint32_t *out_len)
{
    volatile virtq_used_t *us = virtq_used(q);
    __sync_synchronize();
    if (us->idx == q->last_used)
        return 0;
    virtq_used_elem_t e = us->ring[q->last_used % q->num];
    q->last_used++;
    if (out_id)
        *out_id = e.id;
    if (out_len)
        *out_len = e.len;
    return 1;
}

/* Acknowledge a device interrupt (read status, write ack). */
static inline void virtq_irq_ack(virtq_t *q)
{
    uint32_t st = drv_mmio_read32(q->mmio, VMMIO_INTR_STATUS);
    drv_mmio_write32(q->mmio, VMMIO_INTR_ACK, st);
}

#endif
