#include "drivers/block/virtio_blk.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "core/errno.h"

#define VQ_SIZE  VIRTIO_QUEUE_SIZE
#define VIRTIO_BLK_REQ_SLOTS (VIRTIO_QUEUE_SIZE / 3)
#define VIRTIO_BLK_WAIT_TIMEOUT_TICKS (TICKS_PER_SEC * 30)
#define VIRTIO_BLK_MAX_RETRIES        3
#define VIRTIO_BLK_RESET_SPINS        1000000U
#define VIRTIO_BLK_POLL_BACKOFF_MIN_SPINS 32U
#define VIRTIO_BLK_POLL_BACKOFF_MAX_SPINS 512U
#define VIRTIO_BLK_BOUNCE_BYTES           (64U * 1024U)
#define VIRTIO_BLK_MAX_TRANSFER_SECTORS   \
    (VIRTIO_BLK_BOUNCE_BYTES / VIRTIO_BLK_SECTOR_SIZE)
#define VIRTIO_BLK_QUEUE_DMA_BYTES        (PAGE_SIZE * 3U)
#define VIRTIO_BLK_REQUEST_DMA_BYTES      PAGE_SIZE

typedef struct {
    int                in_use;
    int                done;
    int                result;
    int                write;
    uint16_t           head;
    void              *buf;
    void              *dma_buf;
    uint64_t           dma_addr;
    size_t             bytes;
    wait_queue_t       waiters;
} virtio_blk_req_t;

typedef struct {
    virtio_blk_t       blk;
    block_dev_t        blk_dev;
    virtio_transport_t vt;
    void              *queue_dma_mem;
    uint64_t           queue_dma_addr;
    void              *request_dma_mem;
    uint64_t           request_dma_addr;
    virtio_blk_req_hdr_t *req_hdr;
    uint8_t           *status;
    virtio_blk_req_t   req[VIRTIO_BLK_REQ_SLOTS];
    /* LOCK_ORDER: inst->lock is innermost; no nesting under or over other locks.
     * Protects req[], descriptor/avail/used rings, in_flight, status[],
     * req_hdr[], last_used, desc_idx. */
    spinlock_t         lock;
    int                slot;
    int                in_flight;
    int                irq_registered;
} virtio_blk_inst_t;

static virtio_blk_inst_t g_insts[VIRTIO_MAX_DEVS];
static int g_ninst = 0;

static void virtio_blk_free_dma(virtio_blk_inst_t *inst)
{
    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        if (!inst->req[i].dma_buf)
            continue;
        dma_free_coherent_aligned(inst->req[i].dma_buf,
                                  VIRTIO_BLK_BOUNCE_BYTES,
                                  inst->req[i].dma_addr);
        inst->req[i].dma_buf = NULL;
        inst->req[i].dma_addr = 0;
    }
    if (inst->request_dma_mem) {
        dma_free_coherent_aligned(inst->request_dma_mem,
                                  VIRTIO_BLK_REQUEST_DMA_BYTES,
                                  inst->request_dma_addr);
        inst->request_dma_mem = NULL;
        inst->request_dma_addr = 0;
        inst->req_hdr = NULL;
        inst->status = NULL;
    }
    if (inst->queue_dma_mem) {
        dma_free_coherent_aligned(inst->queue_dma_mem,
                                  VIRTIO_BLK_QUEUE_DMA_BYTES,
                                  inst->queue_dma_addr);
        inst->queue_dma_mem = NULL;
        inst->queue_dma_addr = 0;
    }
}

/*
 * Never expose an arbitrary kernel buffer as one VirtIO DMA segment.  Block
 * cache pages and batched writeback buffers are not required to be physically
 * contiguous even when their virtual addresses are contiguous.  Both MyGO
 * and ScintillaOS independently avoid this evaluator-sensitive assumption by
 * using bounded, contiguous bounce storage.  Give every in-flight slot its
 * own 64 KiB DMA buffer so concurrent filesystem requests cannot overwrite
 * each other while the device still owns a descriptor chain.
 */
static int virtio_blk_alloc_dma(virtio_blk_inst_t *inst)
{
    inst->queue_dma_mem =
        dma_alloc_coherent_aligned(VIRTIO_BLK_QUEUE_DMA_BYTES, PAGE_SIZE,
                                   &inst->queue_dma_addr);
    inst->request_dma_mem =
        dma_alloc_coherent_aligned(VIRTIO_BLK_REQUEST_DMA_BYTES, PAGE_SIZE,
                                   &inst->request_dma_addr);
    if (!inst->queue_dma_mem || !inst->request_dma_mem)
        goto fail;

    inst->req_hdr = (virtio_blk_req_hdr_t *)inst->request_dma_mem;
    inst->status = (uint8_t *)inst->request_dma_mem +
                   sizeof(virtio_blk_req_hdr_t) * VIRTIO_QUEUE_SIZE;
    if ((uintptr_t)(inst->status + VIRTIO_QUEUE_SIZE) >
        (uintptr_t)inst->request_dma_mem + VIRTIO_BLK_REQUEST_DMA_BYTES)
        goto fail;

    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        inst->req[i].dma_buf =
            dma_alloc_coherent_aligned(VIRTIO_BLK_BOUNCE_BYTES, PAGE_SIZE,
                                       &inst->req[i].dma_addr);
        if (!inst->req[i].dma_buf)
            goto fail;
    }
    return 0;

fail:
    virtio_blk_free_dma(inst);
    return -1;
}

/*
 * Device reset + feature negotiation + queue setup.  Runs on the DMA buffers
 * allocated once by virtio_blk_alloc_dma(), so it can be called again after
 * an I/O timeout to recover a wedged queue without reallocating memory.
 * Callers either hold inst->lock or run before the instance is live.
 *
 * Completion model: PulseOS-style interrupt-driven completion is the default
 * whenever the transport exposes an IRQ (registered in probe); polling via
 * virtio_blk_poll_inst() remains as the fallback for IRQ-less transports.
 */
static int virtio_blk_device_init_locked(virtio_blk_inst_t *inst) {
    virtio_transport_t *vt = &inst->vt;
    int idx = inst->slot;

    inst->blk.valid  = 0;
    inst->in_flight  = 0;

    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    unsigned spins = VIRTIO_BLK_RESET_SPINS;
    while (vt->read32(vt, VIRTIO_MMIO_STATUS) != 0 && --spins)
        cpu_relax();
    if (!spins) {
        printf("[VIRTIO%d] device did not acknowledge reset\n", idx);
        return -1;
    }

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();

    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, 0);

    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t driver_hi = 0;
    if (!inst->blk.legacy) {
        driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    }
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();

    if (!inst->blk.legacy) {
        status |= VIRTIO_STATUS_FEATURES_OK;
        vt->write32(vt, VIRTIO_MMIO_STATUS, status);
        mb();
        uint32_t s = vt->read32(vt, VIRTIO_MMIO_STATUS);
        if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
            printf("[VIRTIO%d] Device rejected features (hi=0x%x)\n",
                   idx, driver_hi);
            return -1;
        }
    }

    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < VIRTIO_QUEUE_SIZE) {
        printf("[VIRTIO%d] Queue max too small: %d\n", idx, qmax);
        return -1;
    }
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);

    memset(inst->queue_dma_mem, 0, VIRTIO_BLK_QUEUE_DMA_BYTES);
    memset(inst->request_dma_mem, 0, VIRTIO_BLK_REQUEST_DMA_BYTES);
    arch_dma_sync_for_device(inst->queue_dma_mem,
                             VIRTIO_BLK_QUEUE_DMA_BYTES);
    arch_dma_sync_for_device(inst->request_dma_mem,
                             VIRTIO_BLK_REQUEST_DMA_BYTES);

    if (inst->blk.legacy) {
        uint8_t *queue = (uint8_t *)inst->queue_dma_mem;
        virtq_desc_t *l_desc = (virtq_desc_t *)queue;
        virtq_avail_t *l_avail =
            (virtq_avail_t *)(queue + VQ_SIZE * sizeof(virtq_desc_t));
        virtq_used_t *l_used = (virtq_used_t *)(queue + PAGE_SIZE);

        inst->blk.desc  = l_desc;
        inst->blk.avail = l_avail;
        inst->blk.used  = l_used;

        vt->write32(vt, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_PFN,
                    (uint32_t)(inst->queue_dma_addr / PAGE_SIZE));
        mb();
    } else {
        uint8_t *queue = (uint8_t *)inst->queue_dma_mem;
        uint64_t desc_pa = inst->queue_dma_addr;
        uint64_t avail_pa = inst->queue_dma_addr + PAGE_SIZE;
        uint64_t used_pa = inst->queue_dma_addr + PAGE_SIZE * 2U;

        inst->blk.desc = (virtq_desc_t *)queue;
        inst->blk.avail = (virtq_avail_t *)(queue + PAGE_SIZE);
        inst->blk.used = (virtq_used_t *)(queue + PAGE_SIZE * 2U);

        vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)(desc_pa));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_pa  >> 32));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)(avail_pa));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_pa >> 32));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)(used_pa));
        vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_pa  >> 32));
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
        mb();
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();

    uint64_t cap_lo = vt->read32(vt, VIRTIO_MMIO_CONFIG + 0);
    uint64_t cap_hi = vt->read32(vt, VIRTIO_MMIO_CONFIG + 4);
    inst->blk.capacity = cap_lo | (cap_hi << 32);

    inst->blk.desc_idx  = 0;
    inst->blk.last_used = 0;
    inst->blk.valid     = 1;
    return 0;
}

static int virtio_blk_init_instance(virtio_blk_inst_t *inst) {
    int idx = inst->slot;

    inst->blk.legacy = inst->vt.legacy;
    spin_init(&inst->lock);
    if (virtio_blk_alloc_dma(inst) != 0) {
        printf("[VIRTIO%d] Failed to allocate contiguous DMA memory\n", idx);
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&inst->lock);
    int ret = virtio_blk_device_init_locked(inst);
    spin_unlock_irqrestore(&inst->lock, flags);
    if (ret != 0) {
        virtio_blk_free_dma(inst);
        return ret;
    }

    printf("[VIRTIO%d] Block device ready: capacity=%lu sectors (%lu MB)\n",
           idx, (unsigned long)inst->blk.capacity,
           (unsigned long)(inst->blk.capacity / 2048));

    inst->blk_dev.capacity    = inst->blk.capacity;
    inst->blk_dev.sector_size = VIRTIO_BLK_SECTOR_SIZE;
    inst->blk_dev.priv        = inst;
    inst->blk_dev.read_sector = NULL;
    inst->blk_dev.write_sector = NULL;

    return 0;
}

int virtio_blk_init(void) {
    if (g_ninst >= VIRTIO_MAX_DEVS) {
        printf("[VIRTIO] Too many devices (max %d)\n", VIRTIO_MAX_DEVS);
        return -1;
    }

    int idx = g_ninst;
    virtio_blk_inst_t *inst = &g_insts[idx];

    if (arch_virtio_blk_probe(idx, &inst->vt) != 0) {
        printf("[VIRTIO%d] Probe failed\n", idx);
        return -1;
    }

    inst->slot = idx;
    if (virtio_blk_init_instance(inst) != 0)
        return -1;

    g_ninst++;
    return 0;
}

static virtio_blk_req_t *virtio_blk_find_req_locked(virtio_blk_inst_t *inst, uint16_t head) {
    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        virtio_blk_req_t *req = &inst->req[i];
        if (req->in_use && req->head == head)
            return req;
    }
    return NULL;
}

/*
 * VIRTIO_BLK_COMPLETION_MODEL:
 * - Request submission publishes the request and kicks the device under
 *   inst->lock.
 * - Completion drains used-ring entries, records req->done/result, and
 *   detaches waiters into a deferred wake queue.
 * - kernel_progress_poll() invokes the bound device's progress callback as a
 *   compatibility bridge, but scheduler/idle code must not call this driver
 *   directly.
 * - The target model is IRQ or bottom-half completion that invokes the same
 *   wake path without requiring scheduler hot-path polling.
 */
static void virtio_blk_complete_used_locked(virtio_blk_inst_t *inst,
                                            proc_wake_q_t *wake_q) {
    virtio_blk_t *blk = &inst->blk;
    virtq_used_t *used = blk->used;

    a20_perf_count(A20_PERF_VIRTIO_BLK_USED_CHECKS);
    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtq_used_t *)used)->idx;
    rmb();
    uint64_t completions = 0;
    while (blk->last_used != used_idx) {
        completions++;
        uint16_t ring_idx = blk->last_used % VIRTIO_QUEUE_SIZE;
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t head = (uint16_t)used->ring[ring_idx].id;
        uint32_t used_len = used->ring[ring_idx].len;
        virtio_blk_req_t *req = virtio_blk_find_req_locked(inst, head);

        if (req) {
            arch_dma_sync_for_cpu(&inst->status[head], 1);
            if (!req->write)
                arch_dma_sync_for_cpu(req->dma_buf, req->bytes);
            uint32_t expected_len = req->write ? 1U : (uint32_t)req->bytes + 1U;
            req->result =
                (inst->status[head] == VIRTIO_BLK_S_OK &&
                 used_len >= expected_len) ? 0 : -1;
            if (req->result < 0) {
                printf("[VIRTIO%d] bad completion head=%u status=%u "
                       "used_len=%u expected>=%u\n",
                       inst->slot, head, inst->status[head], used_len,
                       expected_len);
            }
            req->done = 1;
            if (wake_q)
                (void)wait_queue_collect_all(&req->waiters, 0,
                                             PROC_WAKE_EVENT, wake_q, NULL);
        }

        blk->last_used++;
    }
    a20_perf_add(A20_PERF_VIRTIO_BLK_COMPLETIONS, completions);
}

static void virtio_blk_poll_inst(virtio_blk_inst_t *inst) {
    a20_perf_count(A20_PERF_VIRTIO_BLK_POLLS);
    if (!inst || !inst->blk.valid)
        return;
    if (__atomic_load_n(&inst->in_flight, __ATOMIC_ACQUIRE) <= 0)
        return;
    a20_perf_count(A20_PERF_VIRTIO_BLK_ACTIVE_POLLS);
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    /* LOCK_ORDER: acquire inst->lock (innermost) for completion polling. */
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (inst->in_flight > 0)
        virtio_blk_complete_used_locked(inst, &wake_q);
    spin_unlock_irqrestore(&inst->lock, flags);
    (void)proc_wake_q_flush(&wake_q);
}

void virtio_blk_poll_all(void) {
    for (int i = 0; i < g_ninst; i++)
        virtio_blk_poll_inst(&g_insts[i]);
}

static virtio_blk_req_t *virtio_blk_alloc_req_locked(virtio_blk_inst_t *inst,
                                                      proc_wake_q_t *wake_q) {
    virtio_blk_complete_used_locked(inst, wake_q);
    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        if (!inst->req[i].in_use) {
            virtio_blk_req_t *req = &inst->req[i];
            void *dma_buf = req->dma_buf;
            uint64_t dma_addr = req->dma_addr;
            memset(req, 0, sizeof(*req));
            req->dma_buf = dma_buf;
            req->dma_addr = dma_addr;
            wait_queue_init(&req->waiters);
            req->in_use = 1;
            req->head = (uint16_t)(i * 3);
            return req;
        }
    }
    return NULL;
}

/*
 * Recover a wedged queue without permanently fencing off the block device.
 * All in-flight requests are failed (-1) so their waiters return and retry
 * through the virtio_blk_rw() retry loop; the device is then reset and
 * re-initialised on the existing DMA buffers and becomes valid again.  Only
 * a failed re-initialisation leaves inst->blk.valid cleared.
 */
static void virtio_blk_fail_queue_locked(virtio_blk_inst_t *inst,
                                         proc_wake_q_t *wake_q) {
    if (!inst->blk.valid)
        return;

    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        virtio_blk_req_t *req = &inst->req[i];
        if (!req->in_use)
            continue;
        req->result = -1;
        req->done = 1;
        if (wake_q)
            (void)wait_queue_collect_all(&req->waiters, 0,
                                         PROC_WAKE_EXIT, wake_q, NULL);
    }

    if (virtio_blk_device_init_locked(inst) != 0)
        printf("[VIRTIO%d] queue recovery failed; device stays disabled\n",
               inst->slot);
    else
        printf("[VIRTIO%d] queue recovered after reset\n", inst->slot);
}

/*
 * PulseOS-style interrupt completion: acknowledge the ISR bits, drain the
 * used ring under inst->lock and wake the parked waiters.  The exact same
 * drain routine is used by the polling fallback, so both completion models
 * share one ownership path.
 */
static int virtio_blk_irq_handler(int irq, void *priv) {
    (void)irq;
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)priv;
    if (!inst)
        return 0;
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!isr)
        return 0;
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (inst->blk.valid)
        virtio_blk_complete_used_locked(inst, &wake_q);
    spin_unlock_irqrestore(&inst->lock, flags);
    (void)proc_wake_q_flush(&wake_q);
    return 0;
}

static int virtio_blk_submit_req(virtio_blk_inst_t *inst, virtio_blk_req_t *req,
                                 uint64_t lba, void *buf, size_t sectors,
                                 int write) {
    size_t bytes = sectors * VIRTIO_BLK_SECTOR_SIZE;
    virtio_transport_t *vt = &inst->vt;
    uint16_t slot = req->head;

    req->done = 0;
    req->result = -1;
    req->write = write;
    req->buf = buf;
    req->bytes = bytes;
    inst->in_flight++;

    if (write)
        memcpy(req->dma_buf, buf, bytes);

    inst->req_hdr[slot].type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    inst->req_hdr[slot].reserved = 0;
    inst->req_hdr[slot].sector   = lba;

    inst->status[slot] = 0xFF;

    virtq_desc_t *desc  = inst->blk.desc;
    virtq_avail_t *avail = inst->blk.avail;

    desc[slot].addr  = inst->request_dma_addr +
                       (uint64_t)slot * sizeof(virtio_blk_req_hdr_t);
    desc[slot].len   = sizeof(virtio_blk_req_hdr_t);
    desc[slot].flags = VIRTQ_DESC_F_NEXT;
    desc[slot].next  = slot + 1;

    desc[slot + 1].addr  = req->dma_addr;
    desc[slot + 1].len   = (uint32_t)bytes;
    desc[slot + 1].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    desc[slot + 1].next  = slot + 2;

    desc[slot + 2].addr  = inst->request_dma_addr +
                           sizeof(virtio_blk_req_hdr_t) * VIRTIO_QUEUE_SIZE +
                           slot;
    desc[slot + 2].len   = 1;
    desc[slot + 2].flags = VIRTQ_DESC_F_WRITE;
    desc[slot + 2].next  = 0;

    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = slot;
    arch_dma_sync_for_device(req->dma_buf, bytes);
    arch_dma_sync_for_device(&inst->req_hdr[slot], sizeof(inst->req_hdr[slot]));
    arch_dma_sync_for_device(&inst->status[slot], 1);
    arch_dma_sync_for_device(&desc[slot], sizeof(virtq_desc_t) * 3);
    arch_dma_sync_for_device(&avail->flags, sizeof(avail->flags));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    wmb();
    avail->idx++;
    inst->blk.desc_idx++;
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
    wmb();

    vt->write32(vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
    return 0;
}

static int virtio_blk_wait_req(virtio_blk_inst_t *inst, virtio_blk_req_t *req,
                               uint64_t lba) {
    task_t *cur = proc_current();
    uint64_t deadline = timer_get_ticks() + VIRTIO_BLK_WAIT_TIMEOUT_TICKS;
    unsigned poll_backoff = VIRTIO_BLK_POLL_BACKOFF_MIN_SPINS;

    for (;;) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        uint64_t flags = spin_lock_irqsave(&inst->lock);
        /* LOCK_ORDER: inst->lock held while checking request completion. */
        if (inst->blk.valid)
            virtio_blk_complete_used_locked(inst, &wake_q);
        if (req->done) {
            int ret = req->result;
            if (ret == 0 && !req->write)
                memcpy(req->buf, req->dma_buf, req->bytes);
            req->in_use = 0;
            if (inst->in_flight > 0)
                inst->in_flight--;
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            return ret;
        }
        if (timer_get_ticks() >= deadline) {
            virtio_blk_complete_used_locked(inst, &wake_q);
            if (req->done) {
                int ret = req->result;
                if (ret == 0 && !req->write)
                    memcpy(req->buf, req->dma_buf, req->bytes);
                req->in_use = 0;
                if (inst->in_flight > 0)
                    inst->in_flight--;
                spin_unlock_irqrestore(&inst->lock, flags);
                (void)proc_wake_q_flush(&wake_q);
                return ret;
            }
            uint32_t dev_status =
                inst->vt.read32(&inst->vt, VIRTIO_MMIO_STATUS);
            uint16_t device_used =
                ((volatile virtq_used_t *)inst->blk.used)->idx;
            uint16_t driver_avail =
                ((volatile virtq_avail_t *)inst->blk.avail)->idx;
            uint16_t last_used = inst->blk.last_used;
            int write = req->write;
            size_t bytes = req->bytes;
            uint64_t dma_addr = req->dma_addr;
            uint16_t head = req->head;
            virtio_blk_fail_queue_locked(inst, &wake_q);
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);

            printf("[VIRTIO%d] I/O timeout op=%s lba=%lu bytes=%lu "
                   "head=%u dma=0x%lx status_before_reset=0x%x "
                   "avail=%u used=%u last_used=%u\n",
                   inst->slot, write ? "write" : "read",
                   (unsigned long)lba, (unsigned long)bytes, head,
                   (unsigned long)dma_addr, dev_status, driver_avail,
                   device_used, last_used);
            printf("[VIRTIO%d] wait_req timeout lba=%lu pid=%d\n",
                   inst->slot, (unsigned long)lba, cur ? cur->pid : -1);
            continue;
        }

        /*
         * Poll-only transports cannot wake a blocked task through an IRQ.
         * Keep draining the used ring here instead of depending on a later
         * scheduler pass to notice the completion.
         */
        if (inst->vt.irq < 0) {
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            /* The used ring was already drained at the top of this iteration.
             * Calling poll_inst() here repeated the same DMA sync and lock;
             * immediately starting the next iteration instead made the guest
             * hammer used->idx hundreds of times per completion.  A short,
             * architecture-neutral backoff keeps latency bounded while giving
             * the QEMU device thread time to publish the used entry. */
            for (unsigned spin = 0; spin < poll_backoff; spin++)
                cpu_relax();
            if (poll_backoff < VIRTIO_BLK_POLL_BACKOFF_MAX_SPINS)
                poll_backoff <<= 1;
            continue;
        }

        if (cur) {
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            proc_wait_token_t token =
                proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, deadline);
            if (!token.task) {
                cpu_relax();
                continue;
            }

            wait_queue_entry_t entry = {0};
            flags = spin_lock_irqsave(&inst->lock);
            if (inst->blk.valid)
                virtio_blk_complete_used_locked(inst, &wake_q);
            if (req->done) {
                spin_unlock_irqrestore(&inst->lock, flags);
                (void)proc_wake_q_flush(&wake_q);
                (void)proc_park_cancel(token);
                proc_park_finish(token);
                continue;
            }
            bool linked =
                wait_queue_link(&req->waiters, &entry, token, 0);
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (linked)
                (void)proc_park_commit(token);
            else
                (void)proc_park_cancel(token);
            wait_queue_unlink(&req->waiters, &entry);
            proc_park_finish(token);
        } else {
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            cpu_relax();
        }
    }
}

static int virtio_blk_rw(int idx, uint64_t lba, void *buf, size_t sectors, int write) {
    if (idx < 0 || idx >= g_ninst) return -1;
    if (!buf || sectors == 0 || sectors > VIRTIO_BLK_MAX_TRANSFER_SECTORS)
        return -EINVAL;
    virtio_blk_inst_t *inst = &g_insts[idx];
    if (!inst->blk.valid) return -1;

    int retries = 0;
    for (retries = 0; retries <= VIRTIO_BLK_MAX_RETRIES; retries++) {
        virtio_blk_req_t *req = NULL;
        uint64_t alloc_deadline = timer_get_ticks() + VIRTIO_BLK_WAIT_TIMEOUT_TICKS;
        while (!req) {
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            /* LOCK_ORDER: acquire inst->lock (innermost) to allocate/submit a request. */
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            req = virtio_blk_alloc_req_locked(inst, &wake_q);
            if (req) {
                virtio_blk_submit_req(inst, req, lba, buf, sectors, write);
                spin_unlock_irqrestore(&inst->lock, flags);
                (void)proc_wake_q_flush(&wake_q);
                break;
            }
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            if (!inst->blk.valid)
                return -1;
            if (timer_get_ticks() >= alloc_deadline) {
                proc_wake_q_init(&wake_q);
                flags = spin_lock_irqsave(&inst->lock);
                virtio_blk_complete_used_locked(inst, &wake_q);
                virtio_blk_fail_queue_locked(inst, &wake_q);
                int valid = inst->blk.valid;
                spin_unlock_irqrestore(&inst->lock, flags);
                (void)proc_wake_q_flush(&wake_q);
                printf("[VIRTIO%d] descriptor allocation timed out; queue reset\n",
                       inst->slot);
                if (!valid)
                    return -1;
                break;
            }
            if (proc_current())
                proc_yield();
            else
                cpu_relax();
        }

        if (!req)
            continue;

        int ret = virtio_blk_wait_req(inst, req, lba);
        if (ret == 0)
            return 0;
        if (!inst->blk.valid)
            return ret;
        if (ret < 0 && retries < VIRTIO_BLK_MAX_RETRIES) {
            printf("[VIRTIO%d] Retrying I/O (%d/%d) lba=%lu\n",
                   idx, retries + 1, VIRTIO_BLK_MAX_RETRIES, (unsigned long)lba);
            /* Brief pause before retry — let any pending completions drain */
            virtio_blk_poll_inst(inst);
            continue;
        }
        uint16_t head = req ? req->head : 0;
        printf("[VIRTIO%d] I/O error: status=%d lba=%lu (after %d retries)\n",
               idx, inst->status[head], (unsigned long)lba, retries);
        return ret;
    }
    return -1;
}

int virtio_blk_read(int idx, uint64_t lba, void *buf, size_t sectors) {
    uint8_t *cursor = (uint8_t *)buf;
    while (sectors) {
        size_t chunk = sectors > VIRTIO_BLK_MAX_TRANSFER_SECTORS ?
            VIRTIO_BLK_MAX_TRANSFER_SECTORS : sectors;
        int ret = virtio_blk_rw(idx, lba, cursor, chunk, 0);
        if (ret < 0)
            return ret;
        lba += chunk;
        cursor += chunk * VIRTIO_BLK_SECTOR_SIZE;
        sectors -= chunk;
    }
    return 0;
}

int virtio_blk_write(int idx, uint64_t lba, const void *buf, size_t sectors) {
    const uint8_t *cursor = (const uint8_t *)buf;
    while (sectors) {
        size_t chunk = sectors > VIRTIO_BLK_MAX_TRANSFER_SECTORS ?
            VIRTIO_BLK_MAX_TRANSFER_SECTORS : sectors;
        int ret = virtio_blk_rw(idx, lba, (void *)cursor, chunk, 1);
        if (ret < 0)
            return ret;
        lba += chunk;
        cursor += chunk * VIRTIO_BLK_SECTOR_SIZE;
        sectors -= chunk;
    }
    return 0;
}

uint64_t virtio_blk_capacity(int idx) {
    if (idx < 0 || idx >= g_ninst) return 0;
    return g_insts[idx].blk.capacity;
}

static int blk_read_sector(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->priv;
    return virtio_blk_read(inst->slot, lba, buf, count);
}

static int blk_write_sector(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->priv;
    return virtio_blk_write(inst->slot, lba, buf, count);
}

block_dev_t *virtio_blk_get_dev(int idx) {
    if (idx < 0 || idx >= g_ninst) return NULL;
    if (!g_insts[idx].blk.valid) return NULL;
    g_insts[idx].blk_dev.read_sector  = blk_read_sector;
    g_insts[idx].blk_dev.write_sector = blk_write_sector;
    return &g_insts[idx].blk_dev;
}

int virtio_blk_ready(int idx) {
    if (idx < 0 || idx >= g_ninst) return 0;
    return g_insts[idx].blk.valid;
}

static uint32_t virtio_blk_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static void virtio_blk_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static int virtio_blk_driver_probe(device_t *dev) {
    if (g_ninst >= VIRTIO_MAX_DEVS) {
        kinfo("[VIRTIO-BLK] Too many devices (max %d)\n", VIRTIO_MAX_DEVS);
        return -1;
    }

    int idx = g_ninst;
    virtio_blk_inst_t *inst = &g_insts[idx];
    memset(inst, 0, sizeof(*inst));
    inst->vt.irq = -1;

    if (dev->bus == &pci_bus) {
        if (pci_virtio_transport_init(dev, 2, &inst->vt) != 0) {
            kinfo("[VIRTIO-BLK] PCI transport setup failed for '%s'\n", dev->name);
            return -1;
        }
    } else {
        resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
        if (!mmio_res) {
            kinfo("[VIRTIO-BLK] No MMIO resource for device '%s'\n", dev->name);
            return -1;
        }
        inst->vt.read32  = virtio_blk_mmio_read32;
        inst->vt.write32 = virtio_blk_mmio_write32;
        inst->vt.priv    = (void *)(uintptr_t)mmio_res->start;
        resource_t *mmio_irq = device_get_resource(dev, RES_IRQ, 0);
        if (mmio_irq)
            inst->vt.irq = (int)mmio_irq->start;
    }

    uint32_t magic   = inst->vt.read32(&inst->vt, VIRTIO_MMIO_MAGIC);
    uint32_t version = inst->vt.read32(&inst->vt, VIRTIO_MMIO_VERSION);
    uint32_t dev_id  = inst->vt.read32(&inst->vt, VIRTIO_MMIO_DEVICE_ID);

    if (magic != 0x74726976 || (version != 1 && version != 2) || dev_id != 2) {
        kinfo("[VIRTIO-BLK] Invalid transport (magic=0x%x version=%d dev_id=%d)\n",
              magic, version, dev_id);
        return -1;
    }

    inst->vt.legacy = (version == 1);
    inst->slot = idx;
    dev->drv_priv = inst;

    int ret = virtio_blk_init_instance(inst);
    if (ret != 0) {
        kinfo("[VIRTIO-BLK] Init failed for device '%s'\n", dev->name);
        return ret;
    }

    if (inst->vt.irq >= 0) {
        if (request_irq((uint32_t)inst->vt.irq, virtio_blk_irq_handler,
                        0, inst) == 0) {
            inst->irq_registered = 1;
            kinfo("[VIRTIO-BLK] %s using IRQ %d completions\n",
                  dev->name, inst->vt.irq);
        } else {
            kinfo("[VIRTIO-BLK] %s IRQ %d registration failed; "
                  "falling back to completion polling\n",
                  dev->name, inst->vt.irq);
            inst->vt.irq = -1;
        }
    }
    if (inst->vt.irq < 0)
        kinfo("[VIRTIO-BLK] %s using completion polling\n", dev->name);

    g_ninst++;
    kinfo("[VIRTIO-BLK] Probed device '%s' (legacy=%d irq=%d)\n",
          dev->name, inst->vt.legacy, inst->vt.irq);
    return 0;
}

static int virtio_blk_class_read(struct device *dev, uint64_t sector, void *buf, size_t count) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return virtio_blk_read(inst->slot, sector, buf, count);
}

static int virtio_blk_class_write(struct device *dev, uint64_t sector, const void *buf, size_t count) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return virtio_blk_write(inst->slot, sector, buf, count);
}

static uint64_t virtio_blk_class_capacity(struct device *dev) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return inst->blk.capacity;
}

static uint32_t virtio_blk_class_sector_size(struct device *dev) {
    (void)dev;
    return VIRTIO_BLK_SECTOR_SIZE;
}

static block_dev_ops_t virtio_blk_class_ops = {
    .read        = virtio_blk_class_read,
    .write       = virtio_blk_class_write,
    .capacity    = virtio_blk_class_capacity,
    .sector_size = virtio_blk_class_sector_size,
};

static const device_id_t virtio_blk_ids[] = {
    /* VirtIO-MMIO bus matching uses the transport device type as device ID. */
    { .vendor = 0, .device = 2,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1002,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    /* QEMU transitional virtio-blk may expose generic 1001 and identify the
     * device type through subsystem ID 2. */
    { .vendor = 0x1AF4, .device = 0x1001,
      .subvendor = VENDOR_ANY, .subdevice = 2 },
    { .vendor = 0x1AF4, .device = 0x1042,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static int virtio_blk_driver_remove(device_t *dev) {
    virtio_blk_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return 0;
    inst->blk.valid = 0;
    if (inst->irq_registered) {
        free_irq((uint32_t)inst->vt.irq, inst);
        inst->irq_registered = 0;
    }
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    inst->in_flight = 0;
    dev->drv_priv = NULL;
    return 0;
}

static void virtio_blk_driver_progress(device_t *dev)
{
    virtio_blk_inst_t *inst = dev ? (virtio_blk_inst_t *)dev->drv_priv : NULL;
    virtio_blk_poll_inst(inst);
}

static driver_t virtio_blk_driver = {
    .name       = "virtio-blk",
    .id_table   = virtio_blk_ids,
    .bus        = NULL,
    .probe      = virtio_blk_driver_probe,
    .remove     = virtio_blk_driver_remove,
    .progress   = virtio_blk_driver_progress,
    .class_ops  = &virtio_blk_class_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(virtio_blk_driver);
