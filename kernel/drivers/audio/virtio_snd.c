/*
 * VirtIO 1.2 sound PCM playback driver
 */
#include "drivers/audio/virtio_snd.h"
#include "drivers/audio/audio_core.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/defs.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"

#define VIRTIO_SND_MAX_STREAMS       16U
#define VIRTIO_SND_TX_SLOTS           8U
#define VIRTIO_SND_BUFFER_BYTES   32768U
#define VIRTIO_SND_PERIOD_BYTES    4096U
#define VIRTIO_SND_FRAME_BYTES        4U
#define VIRTIO_SND_PREBUFFER_PERIODS  2U
#define VIRTIO_SND_CONTROL_REQ_BYTES 64U
#define VIRTIO_SND_RESET_SPINS   1000000U
#define VIRTIO_SND_CONTROL_RESP_BYTES \
    (sizeof(uint32_t) + VIRTIO_SND_MAX_STREAMS * sizeof(virtio_snd_pcm_info_t))

typedef struct virtio_snd_queue_mem {
    virtq_desc_t desc[VIRTIO_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t avail ALIGNED(64);
    virtq_used_t used ALIGNED(64);
} ALIGNED(64) virtio_snd_queue_mem_t;

typedef struct virtio_snd_queue {
    virtio_snd_queue_mem_t *mem;
    uint64_t dma;
    uint16_t last_used;
} virtio_snd_queue_t;

typedef struct virtio_snd_control_dma {
    uint8_t request[VIRTIO_SND_CONTROL_REQ_BYTES] ALIGNED(64);
    uint8_t response[VIRTIO_SND_CONTROL_RESP_BYTES] ALIGNED(64);
} ALIGNED(64) virtio_snd_control_dma_t;

typedef struct virtio_snd_tx_dma_slot {
    virtio_snd_pcm_xfer_t xfer ALIGNED(64);
    uint8_t pcm[VIRTIO_SND_PERIOD_BYTES] ALIGNED(64);
    virtio_snd_pcm_status_t status ALIGNED(64);
} ALIGNED(64) virtio_snd_tx_dma_slot_t;

typedef struct virtio_snd_tx_dma {
    virtio_snd_tx_dma_slot_t slot[VIRTIO_SND_TX_SLOTS];
} ALIGNED(64) virtio_snd_tx_dma_t;

typedef struct virtio_snd_tx_state {
    uint32_t bytes;
    int busy;
} virtio_snd_tx_state_t;

typedef struct virtio_snd_dev {
    virtio_transport_t vt;
    virtio_snd_queue_t queues[VIRTIO_SND_QUEUE_COUNT];
    virtio_snd_control_dma_t *control;
    uint64_t control_dma;
    virtio_snd_event_t *events;
    uint64_t events_dma;
    virtio_snd_tx_dma_t *tx_dma;
    uint64_t tx_dma_handle;
    virtio_snd_tx_state_t tx[VIRTIO_SND_TX_SLOTS];
    uint8_t staging[VIRTIO_SND_PERIOD_BYTES];
    size_t staging_bytes;
    uint32_t stream_id;
    uint32_t inflight;
    int tx_error;
    int prepared;
    int started;
    int valid;
    int broken;
    volatile uint32_t generation;
    mutex_t lock;
    struct virtio_snd_dev *quarantine_next;
} virtio_snd_dev_t;

static virtio_snd_dev_t *g_virtio_snd_quarantine;

_Static_assert(sizeof(virtio_snd_queue_mem_t) <= PAGE_SIZE,
               "VirtIO sound queue memory must fit in one page");
_Static_assert(sizeof(virtio_snd_pcm_info_t) == 32,
               "VirtIO sound PCM info layout mismatch");
_Static_assert(sizeof(virtio_snd_event_t) == 8,
               "VirtIO sound event layout mismatch");

static void virtio_snd_mmio_write32(virtio_transport_t *transport,
                                    uint32_t offset, uint32_t value)
{
    writel(value, (volatile void *)((uintptr_t)transport->priv + offset));
}

static uint32_t virtio_snd_mmio_read32(virtio_transport_t *transport,
                                       uint32_t offset)
{
    return readl((const volatile void *)((uintptr_t)transport->priv + offset));
}

static uint64_t virtio_snd_dma_offset(const void *base, uint64_t dma,
                                      const void *member)
{
    return dma + (uint64_t)((const uint8_t *)member - (const uint8_t *)base);
}

static int virtio_snd_status_error(uint32_t status)
{
    switch (status) {
    case VIRTIO_SND_S_OK:
        return 0;
    case VIRTIO_SND_S_BAD_MSG:
        return -EINVAL;
    case VIRTIO_SND_S_NOT_SUPP:
        return -EOPNOTSUPP;
    default:
        return -EIO;
    }
}

static int virtio_snd_reset(virtio_snd_dev_t *snd)
{
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    unsigned spins = VIRTIO_SND_RESET_SPINS;
    while (snd->vt.read32(&snd->vt, VIRTIO_MMIO_STATUS) != 0 && --spins)
        arch_cpu_relax();
    return spins ? 0 : -ETIMEDOUT;
}

static int virtio_snd_health(virtio_snd_dev_t *snd)
{
    uint32_t status = snd->vt.read32(&snd->vt, VIRTIO_MMIO_STATUS);
    if (status & VIRTIO_STATUS_DEVICE_NEEDS_RESET) {
        (void)virtio_snd_reset(snd);
        snd->broken = 1;
        return -EIO;
    }
    if (status & VIRTIO_STATUS_FAILED) {
        snd->broken = 1;
        return -EIO;
    }
    return snd->broken ? -EIO : 0;
}

static void virtio_snd_ack_interrupt(virtio_snd_dev_t *snd)
{
    uint32_t status = snd->vt.read32(&snd->vt,
                                    VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!snd->vt.legacy && status)
        snd->vt.write32(&snd->vt, VIRTIO_MMIO_INTERRUPT_ACK, status);
}

static void virtio_snd_wait_step(void)
{
    if (proc_current())
        proc_yield();
    else
        arch_cpu_relax();
}

static int virtio_snd_wait_used(virtio_snd_dev_t *snd,
                                virtio_snd_queue_t *queue,
                                uint16_t before)
{
    uint64_t start = clock_get_ticks();
    uint64_t timeout = clock_ticks_per_sec();
    if (!timeout)
        timeout = 1;

    for (;;) {
        if (virtio_snd_health(snd) < 0)
            return -EIO;
        dma_sync_for_cpu(&queue->mem->used, sizeof(queue->mem->used));
        if (((volatile virtq_used_t *)&queue->mem->used)->idx != before)
            return 0;
        if (clock_get_ticks() - start >= timeout) {
            snd->broken = 1;
            return -ETIMEDOUT;
        }
        virtio_snd_wait_step();
    }
}

static int virtio_snd_setup_queue(virtio_snd_dev_t *snd, uint32_t index)
{
    virtio_snd_queue_t *queue = &snd->queues[index];
    queue->mem = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE,
                                             &queue->dma);
    if (!queue->mem)
        return -ENOMEM;

    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_SEL, index);
    if (snd->vt.read32(&snd->vt, VIRTIO_MMIO_QUEUE_NUM_MAX) <
        VIRTIO_QUEUE_SIZE ||
        snd->vt.read32(&snd->vt, VIRTIO_MMIO_QUEUE_READY) != 0)
        return -ENODEV;
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);

    dma_sync_for_device(queue->mem, sizeof(*queue->mem));
    uint64_t desc = virtio_snd_dma_offset(queue->mem, queue->dma,
                                          queue->mem->desc);
    uint64_t avail = virtio_snd_dma_offset(queue->mem, queue->dma,
                                           &queue->mem->avail);
    uint64_t used = virtio_snd_dma_offset(queue->mem, queue->dma,
                                          &queue->mem->used);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DESC_HIGH,
                    (uint32_t)(desc >> 32));
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW,
                    (uint32_t)avail);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
                    (uint32_t)(avail >> 32));
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW,
                    (uint32_t)used);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
                    (uint32_t)(used >> 32));
    mb();
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_READY, 1);
    mb();
    return 0;
}

static void virtio_snd_free_dma(virtio_snd_dev_t *snd)
{
    if (!snd)
        return;
    if (snd->tx_dma)
        dma_free_coherent_aligned(snd->tx_dma, sizeof(*snd->tx_dma),
                                  snd->tx_dma_handle);
    if (snd->events)
        dma_free_coherent_aligned(snd->events, PAGE_SIZE, snd->events_dma);
    if (snd->control)
        dma_free_coherent_aligned(snd->control, PAGE_SIZE,
                                  snd->control_dma);
    for (uint32_t i = 0; i < VIRTIO_SND_QUEUE_COUNT; i++) {
        if (snd->queues[i].mem)
            dma_free_coherent_aligned(snd->queues[i].mem, PAGE_SIZE,
                                      snd->queues[i].dma);
    }
}

static void virtio_snd_quarantine(virtio_snd_dev_t *snd)
{
    /* A device that did not acknowledge reset may still own every DMA buffer. */
    snd->quarantine_next = g_virtio_snd_quarantine;
    g_virtio_snd_quarantine = snd;
}

static int virtio_snd_alloc_dma(virtio_snd_dev_t *snd)
{
    for (uint32_t i = 0; i < VIRTIO_SND_QUEUE_COUNT; i++) {
        int ret = virtio_snd_setup_queue(snd, i);
        if (ret < 0)
            return ret;
    }
    snd->control = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE,
                                               &snd->control_dma);
    snd->events = dma_alloc_coherent_aligned(PAGE_SIZE, PAGE_SIZE,
                                              &snd->events_dma);
    snd->tx_dma = dma_alloc_coherent_aligned(sizeof(*snd->tx_dma), PAGE_SIZE,
                                              &snd->tx_dma_handle);
    return snd->control && snd->events && snd->tx_dma ? 0 : -ENOMEM;
}

static void virtio_snd_publish(virtio_snd_dev_t *snd,
                               virtio_snd_queue_t *queue, uint16_t head,
                               uint32_t queue_index)
{
    uint16_t slot = queue->mem->avail.idx % VIRTIO_QUEUE_SIZE;
    queue->mem->avail.ring[slot] = head;
    wmb();
    queue->mem->avail.idx++;
    wmb();
    dma_sync_for_device(&queue->mem->avail, sizeof(queue->mem->avail));
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_NOTIFY, queue_index);
    mb();
}

static int virtio_snd_control(virtio_snd_dev_t *snd, const void *request,
                              size_t request_bytes, size_t response_bytes)
{
    if (!request || !request_bytes ||
        request_bytes > sizeof(snd->control->request) ||
        response_bytes < sizeof(uint32_t) ||
        response_bytes > sizeof(snd->control->response))
        return -EINVAL;
    if (virtio_snd_health(snd) < 0)
        return -EIO;

    virtio_snd_queue_t *queue = &snd->queues[VIRTIO_SND_QUEUE_CONTROL];
    dma_sync_for_cpu(&queue->mem->used, sizeof(queue->mem->used));
    uint16_t before = ((volatile virtq_used_t *)&queue->mem->used)->idx;
    if (before != queue->last_used) {
        snd->broken = 1;
        return -EIO;
    }

    memcpy(snd->control->request, request, request_bytes);
    memset(snd->control->response, 0, response_bytes);
    queue->mem->desc[0].addr = virtio_snd_dma_offset(
        snd->control, snd->control_dma, snd->control->request);
    queue->mem->desc[0].len = (uint32_t)request_bytes;
    queue->mem->desc[0].flags = VIRTQ_DESC_F_NEXT;
    queue->mem->desc[0].next = 1;
    queue->mem->desc[1].addr = virtio_snd_dma_offset(
        snd->control, snd->control_dma, snd->control->response);
    queue->mem->desc[1].len = (uint32_t)response_bytes;
    queue->mem->desc[1].flags = VIRTQ_DESC_F_WRITE;
    queue->mem->desc[1].next = 0;
    dma_sync_for_device(&queue->mem->desc[0], 2 * sizeof(virtq_desc_t));
    dma_sync_for_device(snd->control->request, request_bytes);
    dma_sync_for_device(snd->control->response, response_bytes);
    virtio_snd_publish(snd, queue, 0, VIRTIO_SND_QUEUE_CONTROL);

    int ret = virtio_snd_wait_used(snd, queue, before);
    if (ret < 0)
        return ret;
    dma_sync_for_cpu(&queue->mem->used, sizeof(queue->mem->used));
    uint16_t after = ((volatile virtq_used_t *)&queue->mem->used)->idx;
    virtq_used_elem_t *used = &queue->mem->used.ring[
        before % VIRTIO_QUEUE_SIZE];
    if ((uint16_t)(after - before) != 1U || used->id != 0 ||
        used->len < sizeof(uint32_t) || used->len > response_bytes) {
        snd->broken = 1;
        return -EIO;
    }
    queue->last_used = after;
    dma_sync_for_cpu(snd->control->response, response_bytes);
    virtio_snd_ack_interrupt(snd);

    uint32_t status;
    memcpy(&status, snd->control->response, sizeof(status));
    int status_error = virtio_snd_status_error(status);
    if (status_error < 0)
        return status_error;
    if (used->len != response_bytes) {
        snd->broken = 1;
        return -EIO;
    }
    return 0;
}

static void virtio_snd_populate_events(virtio_snd_dev_t *snd)
{
    virtio_snd_queue_t *queue = &snd->queues[VIRTIO_SND_QUEUE_EVENT];
    for (uint16_t i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        queue->mem->desc[i].addr = snd->events_dma +
                                   (uint64_t)i * sizeof(snd->events[i]);
        queue->mem->desc[i].len = sizeof(snd->events[i]);
        queue->mem->desc[i].flags = VIRTQ_DESC_F_WRITE;
        queue->mem->desc[i].next = 0;
        queue->mem->avail.ring[queue->mem->avail.idx %
                               VIRTIO_QUEUE_SIZE] = i;
        queue->mem->avail.idx++;
    }
    dma_sync_for_device(snd->events,
                        VIRTIO_QUEUE_SIZE * sizeof(snd->events[0]));
    dma_sync_for_device(queue->mem->desc, sizeof(queue->mem->desc));
    dma_sync_for_device(&queue->mem->avail, sizeof(queue->mem->avail));
}

static int virtio_snd_poll_events(virtio_snd_dev_t *snd)
{
    virtio_snd_queue_t *queue = &snd->queues[VIRTIO_SND_QUEUE_EVENT];
    dma_sync_for_cpu(&queue->mem->used, sizeof(queue->mem->used));
    uint16_t used_idx = ((volatile virtq_used_t *)&queue->mem->used)->idx;
    int requeued = 0;

    while (queue->last_used != used_idx) {
        virtq_used_elem_t *used = &queue->mem->used.ring[
            queue->last_used % VIRTIO_QUEUE_SIZE];
        if (used->id >= VIRTIO_QUEUE_SIZE ||
            used->len != sizeof(virtio_snd_event_t)) {
            snd->broken = 1;
            return -EIO;
        }
        uint16_t id = (uint16_t)used->id;
        virtio_snd_event_t *event = &snd->events[id];
        dma_sync_for_cpu(event, sizeof(*event));
        if (event->hdr.code == VIRTIO_SND_EVT_PCM_XRUN &&
            event->data == snd->stream_id)
            snd->tx_error = -EIO;
        dma_sync_for_device(event, sizeof(*event));
        queue->mem->avail.ring[queue->mem->avail.idx %
                               VIRTIO_QUEUE_SIZE] = id;
        queue->mem->avail.idx++;
        queue->last_used++;
        requeued = 1;
    }
    if (requeued) {
        dma_sync_for_device(&queue->mem->avail, sizeof(queue->mem->avail));
        snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_NOTIFY,
                        VIRTIO_SND_QUEUE_EVENT);
        mb();
        virtio_snd_ack_interrupt(snd);
    }
    return 0;
}

static int virtio_snd_tx_reap(virtio_snd_dev_t *snd)
{
    if (virtio_snd_health(snd) < 0 || virtio_snd_poll_events(snd) < 0)
        return -EIO;
    virtio_snd_queue_t *queue = &snd->queues[VIRTIO_SND_QUEUE_TX];
    dma_sync_for_cpu(&queue->mem->used, sizeof(queue->mem->used));
    uint16_t used_idx = ((volatile virtq_used_t *)&queue->mem->used)->idx;
    int completed = 0;

    while (queue->last_used != used_idx) {
        virtq_used_elem_t *used = &queue->mem->used.ring[
            queue->last_used % VIRTIO_QUEUE_SIZE];
        if (used->id >= VIRTIO_SND_TX_SLOTS * 3U ||
            (used->id % 3U) != 0 ||
            used->len < sizeof(virtio_snd_pcm_status_t)) {
            snd->broken = 1;
            return -EIO;
        }
        uint32_t slot = used->id / 3U;
        if (!snd->tx[slot].busy) {
            snd->broken = 1;
            return -EIO;
        }
        dma_sync_for_cpu(&snd->tx_dma->slot[slot].status,
                         sizeof(snd->tx_dma->slot[slot].status));
        int status_error = virtio_snd_status_error(
            snd->tx_dma->slot[slot].status.status);
        if (status_error < 0 && snd->tx_error == 0)
            snd->tx_error = status_error;
        snd->tx[slot].busy = 0;
        snd->tx[slot].bytes = 0;
        if (!snd->inflight) {
            snd->broken = 1;
            return -EIO;
        }
        snd->inflight--;
        queue->last_used++;
        completed++;
    }
    if (completed)
        virtio_snd_ack_interrupt(snd);
    return completed;
}

static int virtio_snd_find_tx_slot(virtio_snd_dev_t *snd)
{
    for (uint32_t i = 0; i < VIRTIO_SND_TX_SLOTS; i++) {
        if (!snd->tx[i].busy)
            return (int)i;
    }
    return -1;
}

static int virtio_snd_wait_tx_slot(virtio_snd_dev_t *snd,
                                   uint32_t generation, int interruptible)
{
    uint64_t start = clock_get_ticks();
    uint64_t timeout = clock_ticks_per_sec();
    if (!timeout)
        timeout = 1;

    for (;;) {
        if (interruptible &&
            __atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
            return -EINTR;
        int ret = virtio_snd_tx_reap(snd);
        if (ret < 0)
            return ret;
        if (snd->tx_error)
            return snd->tx_error;
        int slot = virtio_snd_find_tx_slot(snd);
        if (slot >= 0)
            return slot;
        if (clock_get_ticks() - start >= timeout) {
            snd->broken = 1;
            return -ETIMEDOUT;
        }
        virtio_snd_wait_step();
    }
}

static int virtio_snd_submit_staging(virtio_snd_dev_t *snd, size_t bytes,
                                     uint32_t generation, int interruptible)
{
    if (!bytes || bytes > VIRTIO_SND_PERIOD_BYTES ||
        (bytes % VIRTIO_SND_FRAME_BYTES))
        return -EINVAL;
    int slot = virtio_snd_wait_tx_slot(snd, generation, interruptible);
    if (slot < 0)
        return slot;

    virtio_snd_tx_dma_slot_t *dma_slot = &snd->tx_dma->slot[slot];
    dma_slot->xfer.stream_id = snd->stream_id;
    memcpy(dma_slot->pcm, snd->staging, bytes);
    memset(&dma_slot->status, 0, sizeof(dma_slot->status));

    uint16_t head = (uint16_t)slot * 3U;
    virtio_snd_queue_t *queue = &snd->queues[VIRTIO_SND_QUEUE_TX];
    queue->mem->desc[head].addr = virtio_snd_dma_offset(
        snd->tx_dma, snd->tx_dma_handle, &dma_slot->xfer);
    queue->mem->desc[head].len = sizeof(dma_slot->xfer);
    queue->mem->desc[head].flags = VIRTQ_DESC_F_NEXT;
    queue->mem->desc[head].next = head + 1U;
    queue->mem->desc[head + 1U].addr = virtio_snd_dma_offset(
        snd->tx_dma, snd->tx_dma_handle, dma_slot->pcm);
    queue->mem->desc[head + 1U].len = (uint32_t)bytes;
    queue->mem->desc[head + 1U].flags = VIRTQ_DESC_F_NEXT;
    queue->mem->desc[head + 1U].next = head + 2U;
    queue->mem->desc[head + 2U].addr = virtio_snd_dma_offset(
        snd->tx_dma, snd->tx_dma_handle, &dma_slot->status);
    queue->mem->desc[head + 2U].len = sizeof(dma_slot->status);
    queue->mem->desc[head + 2U].flags = VIRTQ_DESC_F_WRITE;
    queue->mem->desc[head + 2U].next = 0;

    dma_sync_for_device(&queue->mem->desc[head],
                        3U * sizeof(virtq_desc_t));
    dma_sync_for_device(&dma_slot->xfer, sizeof(dma_slot->xfer));
    dma_sync_for_device(dma_slot->pcm, bytes);
    dma_sync_for_device(&dma_slot->status, sizeof(dma_slot->status));
    snd->tx[slot].busy = 1;
    snd->tx[slot].bytes = (uint32_t)bytes;
    snd->inflight++;
    virtio_snd_publish(snd, queue, head, VIRTIO_SND_QUEUE_TX);
    snd->staging_bytes = 0;
    return 0;
}

static int virtio_snd_pcm_command(virtio_snd_dev_t *snd, uint32_t command)
{
    virtio_snd_pcm_hdr_t request;
    memset(&request, 0, sizeof(request));
    request.hdr.code = command;
    request.stream_id = snd->stream_id;
    return virtio_snd_control(snd, &request, sizeof(request),
                               sizeof(uint32_t));
}

static int virtio_snd_prepare(virtio_snd_dev_t *snd, uint32_t generation,
                              int interruptible)
{
    if (snd->prepared)
        return 0;
    if (snd->inflight)
        return -EIO;

    virtio_snd_pcm_set_params_t request;
    memset(&request, 0, sizeof(request));
    request.hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    request.hdr.stream_id = snd->stream_id;
    request.buffer_bytes = VIRTIO_SND_BUFFER_BYTES;
    request.period_bytes = VIRTIO_SND_PERIOD_BYTES;
    request.features = 0;
    request.channels = 2;
    request.format = VIRTIO_SND_PCM_FMT_S16;
    request.rate = VIRTIO_SND_PCM_RATE_48000;
    int ret = virtio_snd_control(snd, &request, sizeof(request),
                                  sizeof(uint32_t));
    if (ret < 0)
        return ret;
    if (interruptible &&
        __atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
        return -EINTR;
    ret = virtio_snd_pcm_command(snd, VIRTIO_SND_R_PCM_PREPARE);
    if (ret < 0)
        return ret;
    snd->prepared = 1;
    snd->started = 0;
    snd->tx_error = 0;
    if (interruptible &&
        __atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
        return -EINTR;
    return 0;
}

static int virtio_snd_start(virtio_snd_dev_t *snd, uint32_t generation,
                            int interruptible)
{
    if (snd->started)
        return 0;
    if (!snd->prepared || !snd->inflight)
        return -EINVAL;
    int ret = virtio_snd_pcm_command(snd, VIRTIO_SND_R_PCM_START);
    if (ret == 0)
        snd->started = 1;
    if (ret == 0 && interruptible &&
        __atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
        return -EINTR;
    return ret;
}

static int virtio_snd_wait_all_tx(virtio_snd_dev_t *snd,
                                  uint32_t generation, int interruptible)
{
    uint64_t start = clock_get_ticks();
    uint64_t timeout = clock_ticks_per_sec();
    if (!timeout)
        timeout = 1;

    while (snd->inflight) {
        if (interruptible &&
            __atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
            return -EINTR;
        int ret = virtio_snd_tx_reap(snd);
        if (ret < 0)
            return ret;
        if (!snd->inflight)
            break;
        if (clock_get_ticks() - start >= timeout) {
            snd->broken = 1;
            return -ETIMEDOUT;
        }
        virtio_snd_wait_step();
    }
    return snd->tx_error;
}

static void virtio_snd_clear_stream(virtio_snd_dev_t *snd)
{
    snd->staging_bytes = 0;
    snd->inflight = 0;
    snd->tx_error = 0;
    snd->prepared = 0;
    snd->started = 0;
    memset(snd->tx, 0, sizeof(snd->tx));
}

static int virtio_snd_release_locked(virtio_snd_dev_t *snd,
                                     uint32_t generation,
                                     int interruptible)
{
    int result = 0;
    int ret;

    if (!snd->prepared)
        return 0;
    if (snd->started) {
        ret = virtio_snd_pcm_command(snd, VIRTIO_SND_R_PCM_STOP);
        if (ret < 0) {
            snd->broken = 1;
            return ret;
        }
        snd->started = 0;
    }

    ret = virtio_snd_pcm_command(snd, VIRTIO_SND_R_PCM_RELEASE);
    if (ret < 0) {
        snd->broken = 1;
        return ret;
    }
    snd->prepared = 0;

    ret = virtio_snd_wait_all_tx(snd, generation, interruptible);
    if (ret < 0)
        result = ret;
    if (!snd->inflight && !snd->broken)
        virtio_snd_clear_stream(snd);
    return result;
}

static int virtio_snd_stop_locked(virtio_snd_dev_t *snd,
                                  uint32_t generation)
{
    int result = 0;
    snd->staging_bytes = 0;
    int ret = virtio_snd_tx_reap(snd);
    if (ret < 0)
        result = ret;
    if (!snd->prepared) {
        if (!snd->inflight && !snd->broken)
            virtio_snd_clear_stream(snd);
        return result;
    }
    ret = virtio_snd_release_locked(snd, generation, 0);
    if (ret < 0 && result == 0)
        result = ret;
    return result;
}

static int virtio_snd_write(device_t *dev, const void *buffer, size_t count)
{
    virtio_snd_dev_t *snd = dev ? dev->drv_priv : NULL;
    if (!snd || !buffer || !count || count > 0x7fffffffU)
        return -EINVAL;

    mutex_lock(&snd->lock);
    if (!snd->valid || snd->broken) {
        mutex_unlock(&snd->lock);
        return -ENODEV;
    }
    uint32_t generation = __atomic_load_n(&snd->generation,
                                           __ATOMIC_ACQUIRE);
    int ret = virtio_snd_prepare(snd, generation, 1);
    size_t consumed = 0;
    const uint8_t *input = buffer;

    while (ret == 0 && consumed < count) {
        if (__atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation) {
            ret = -EINTR;
            break;
        }
        size_t chunk = VIRTIO_SND_PERIOD_BYTES - snd->staging_bytes;
        if (chunk > count - consumed)
            chunk = count - consumed;
        memcpy(snd->staging + snd->staging_bytes, input + consumed, chunk);
        snd->staging_bytes += chunk;
        consumed += chunk;
        if (snd->staging_bytes == VIRTIO_SND_PERIOD_BYTES) {
            ret = virtio_snd_submit_staging(snd, snd->staging_bytes,
                                            generation, 1);
            if (ret == 0 && !snd->started &&
                snd->inflight >= VIRTIO_SND_PREBUFFER_PERIODS)
                ret = virtio_snd_start(snd, generation, 1);
        }
    }
    if (__atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation)
        ret = -EINTR;
    mutex_unlock(&snd->lock);
    if (ret == -EINTR)
        return ret;
    return ret < 0 && !consumed ? ret : (int)consumed;
}

static int virtio_snd_stop_device(device_t *dev)
{
    virtio_snd_dev_t *snd = dev ? dev->drv_priv : NULL;
    if (!snd)
        return -ENODEV;
    uint32_t generation = __atomic_add_fetch(&snd->generation, 1U,
                                              __ATOMIC_ACQ_REL);
    mutex_lock(&snd->lock);
    int ret = snd->valid ? virtio_snd_stop_locked(snd, generation) : -ENODEV;
    mutex_unlock(&snd->lock);
    return ret;
}

static int virtio_snd_drain_device(device_t *dev)
{
    virtio_snd_dev_t *snd = dev ? dev->drv_priv : NULL;
    if (!snd)
        return -ENODEV;
    mutex_lock(&snd->lock);
    if (!snd->valid || snd->broken) {
        mutex_unlock(&snd->lock);
        return -ENODEV;
    }
    uint32_t generation = __atomic_load_n(&snd->generation,
                                           __ATOMIC_ACQUIRE);
    int result = 0;

    if (snd->staging_bytes) {
        size_t bytes = ROUND_UP(snd->staging_bytes, VIRTIO_SND_FRAME_BYTES);
        if (bytes != snd->staging_bytes)
            memset(snd->staging + snd->staging_bytes, 0,
                   bytes - snd->staging_bytes);
        result = virtio_snd_submit_staging(snd, bytes, generation, 1);
    }
    if (result != -EINTR && !snd->broken && snd->inflight && !snd->started) {
        int start_ret = virtio_snd_start(snd, generation, 1);
        if (start_ret < 0 && result == 0)
            result = start_ret;
    }
    if (result != -EINTR && !snd->broken && snd->inflight && snd->started) {
        int wait_ret = virtio_snd_wait_all_tx(snd, generation, 1);
        if (wait_ret < 0 && result == 0)
            result = wait_ret;
        if (wait_ret == -EINTR)
            result = wait_ret;
    }
    if (result == -EINTR) {
        mutex_unlock(&snd->lock);
        return result;
    }

    if (__atomic_load_n(&snd->generation, __ATOMIC_ACQUIRE) != generation) {
        mutex_unlock(&snd->lock);
        return -EINTR;
    }
    if (snd->prepared && !snd->broken) {
        int release_ret = virtio_snd_release_locked(snd, generation, 1);
        if (release_ret < 0 && result == 0)
            result = release_ret;
        if (release_ret == -EINTR)
            result = release_ret;
    }
    mutex_unlock(&snd->lock);
    return result;
}

static int virtio_snd_close(device_t *dev)
{
    virtio_snd_dev_t *snd = dev ? dev->drv_priv : NULL;
    return snd && snd->valid ? 0 : -ENODEV;
}

static int virtio_snd_discover_stream(virtio_snd_dev_t *snd,
                                      uint32_t streams)
{
    virtio_snd_query_info_t request;
    memset(&request, 0, sizeof(request));
    request.hdr.code = VIRTIO_SND_R_PCM_INFO;
    request.start_id = 0;
    request.count = streams;
    request.size = sizeof(virtio_snd_pcm_info_t);
    size_t response_bytes = sizeof(uint32_t) +
                            streams * sizeof(virtio_snd_pcm_info_t);
    int ret = virtio_snd_control(snd, &request, sizeof(request),
                                  response_bytes);
    if (ret < 0)
        return ret;

    const uint8_t *data = snd->control->response + sizeof(uint32_t);
    for (uint32_t i = 0; i < streams; i++) {
        virtio_snd_pcm_info_t info;
        memcpy(&info, data + i * sizeof(info), sizeof(info));
        if (info.direction == VIRTIO_SND_D_OUTPUT &&
            info.channels_min <= 2U && info.channels_max >= 2U &&
            (info.formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) &&
            (info.rates & (1ULL << VIRTIO_SND_PCM_RATE_48000))) {
            snd->stream_id = i;
            return 0;
        }
    }
    return -ENODEV;
}

static int virtio_snd_init_transport(device_t *dev,
                                     const virtio_transport_t *transport)
{
    virtio_snd_dev_t *snd = kcalloc(1, sizeof(*snd));
    if (!snd)
        return -ENOMEM;
    snd->vt = *transport;
    mutex_init(&snd->lock);

    uint32_t magic = snd->vt.read32(&snd->vt, VIRTIO_MMIO_MAGIC);
    uint32_t version = snd->vt.read32(&snd->vt, VIRTIO_MMIO_VERSION);
    uint32_t device_id = snd->vt.read32(&snd->vt, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U ||
        device_id != VIRTIO_SND_DEVICE_ID || snd->vt.legacy)
        goto fail_nodev;

    if (virtio_snd_reset(snd) < 0)
        goto fail_nodev;
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_STATUS, status);
    mb();

    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    snd->vt.read32(&snd->vt, VIRTIO_MMIO_DEVICE_FEATURES);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = snd->vt.read32(&snd->vt,
                                          VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT))
        goto fail;
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_DRIVER_FEATURES,
                    VIRTIO_F_VERSION_1_BIT);
    mb();
    status |= VIRTIO_STATUS_FEATURES_OK;
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_STATUS, status);
    mb();
    if (!(snd->vt.read32(&snd->vt, VIRTIO_MMIO_STATUS) &
          VIRTIO_STATUS_FEATURES_OK))
        goto fail;

    uint32_t streams = snd->vt.read32(&snd->vt,
                                      VIRTIO_MMIO_CONFIG +
                                      VIRTIO_SND_CONFIG_STREAMS);
    if (!streams || streams > VIRTIO_SND_MAX_STREAMS)
        goto fail;
    if (virtio_snd_alloc_dma(snd) < 0)
        goto fail;
    virtio_snd_populate_events(snd);

    status |= VIRTIO_STATUS_DRIVER_OK;
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_STATUS, status);
    mb();
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_QUEUE_NOTIFY,
                    VIRTIO_SND_QUEUE_EVENT);
    mb();
    if (virtio_snd_discover_stream(snd, streams) < 0)
        goto fail;

    snd->valid = 1;
    dev->drv_priv = snd;
    kinfo("[VIRTIO-SND] %s stream=%u, 48000 Hz stereo S16_LE\n",
          dev->name, snd->stream_id);
    return 0;

fail:
    status = snd->vt.read32(&snd->vt, VIRTIO_MMIO_STATUS);
    snd->vt.write32(&snd->vt, VIRTIO_MMIO_STATUS,
                    status | VIRTIO_STATUS_FAILED);
    mb();
    if (virtio_snd_reset(snd) < 0) {
        kerr("[VIRTIO-SND] reset timeout; retaining live DMA\n");
        virtio_snd_quarantine(snd);
        return -EIO;
    }
fail_nodev:
    virtio_snd_free_dma(snd);
    kfree(snd);
    return -ENODEV;
}

static int virtio_snd_probe(device_t *dev)
{
    if (dev->bus == &pci_bus) {
        virtio_transport_t transport;
        if (pci_virtio_transport_init(dev, VIRTIO_SND_DEVICE_ID,
                                      &transport) != 0)
            return -ENODEV;
        return virtio_snd_init_transport(dev, &transport);
    }

    resource_t *mmio = device_get_resource(dev, RES_MMIO, 0);
    if (!mmio)
        return -ENODEV;
    virtio_transport_t transport = {
        .read32 = virtio_snd_mmio_read32,
        .write32 = virtio_snd_mmio_write32,
        .priv = (void *)(uintptr_t)mmio->start,
        .legacy = 0,
        .irq = -1,
    };
    return virtio_snd_init_transport(dev, &transport);
}

static int virtio_snd_remove(device_t *dev)
{
    virtio_snd_dev_t *snd = dev ? dev->drv_priv : NULL;
    if (!snd)
        return 0;
    snd->valid = 0;
    uint32_t generation = __atomic_add_fetch(&snd->generation, 1U,
                                              __ATOMIC_ACQ_REL);
    mutex_lock(&snd->lock);
    (void)virtio_snd_stop_locked(snd, generation);
    mutex_unlock(&snd->lock);

    if (virtio_snd_reset(snd) < 0) {
        kerr("[VIRTIO-SND] %s reset timeout; retaining live DMA\n",
             dev->name);
        virtio_snd_quarantine(snd);
        dev->drv_priv = NULL;
        return -EIO;
    }
    dev->drv_priv = NULL;
    virtio_snd_free_dma(snd);
    kfree(snd);
    return 0;
}

static const audio_dev_ops_t virtio_snd_ops = {
    .caps = {
        .version = 1,
        .flags = A20_AUDIO_CAP_PCM,
        .min_rate = 48000,
        .max_rate = 48000,
    },
    .pcm_format = {
        .rate = 48000,
        .channels = 2,
        .format = A20_AUDIO_FORMAT_S16_LE,
    },
    .write = virtio_snd_write,
    .stop = virtio_snd_stop_device,
    .drain = virtio_snd_drain_device,
    .close = virtio_snd_close,
};

static const device_id_t virtio_snd_ids[] = {
    { .vendor = 0, .device = VIRTIO_SND_DEVICE_ID,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1059,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1019,
      .subvendor = VENDOR_ANY, .subdevice = VIRTIO_SND_DEVICE_ID },
    { 0 },
};

static driver_t virtio_snd_driver = {
    .name = "virtio-snd",
    .id_table = virtio_snd_ids,
    .bus = NULL,
    .probe = virtio_snd_probe,
    .remove = virtio_snd_remove,
    .class_ops = &virtio_snd_ops,
    .class_type = DEV_CLASS_AUDIO,
};

DRIVER_REGISTER(virtio_snd_driver);
