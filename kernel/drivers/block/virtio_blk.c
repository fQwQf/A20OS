#include "drivers/block/virtio_blk.h"
#include "drivers/block/virtio_blk_core.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/cpu.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/progress.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "core/errno.h"

_Static_assert(sizeof(virtq_desc_t) == sizeof(a20_virtio_blk_desc_t),
               "shared VirtIO descriptor size");
_Static_assert(__builtin_offsetof(virtq_desc_t, addr) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, addr) &&
               __builtin_offsetof(virtq_desc_t, len) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, len) &&
               __builtin_offsetof(virtq_desc_t, flags) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, flags) &&
               __builtin_offsetof(virtq_desc_t, next) ==
                   __builtin_offsetof(a20_virtio_blk_desc_t, next),
               "shared VirtIO descriptor offsets");

#define VQ_STORAGE_SIZE VIRTIO_QUEUE_MAX_SIZE
#define VIRTIO_BLK_MAX_REQ_SLOTS (VIRTIO_QUEUE_MAX_SIZE / 3)
#define VIRTIO_BLK_WAIT_TIMEOUT_TICKS (clock_ticks_per_sec() * 30)
#define VIRTIO_BLK_MAX_RETRIES        3
#define VIRTIO_BLK_RESET_SPINS        1000000U
#define VIRTIO_BLK_POLL_BACKOFF_MIN_SPINS 4U
#define VIRTIO_BLK_POLL_BACKOFF_MAX_SPINS 128U
#define VIRTIO_BLK_BOUNCE_BYTES           (64U * 1024U)
#define VIRTIO_BLK_MAX_TRANSFER_BYTES     (1024U * 1024U)
#define VIRTIO_BLK_MAX_TRANSFER_SECTORS   \
    (VIRTIO_BLK_MAX_TRANSFER_BYTES / VIRTIO_BLK_SECTOR_SIZE)
#define VIRTIO_BLK_QUEUE_DMA_BYTES        (PAGE_SIZE * 3U)
#define VIRTIO_BLK_REQUEST_DMA_BYTES      PAGE_SIZE
/* One request queue, so one message-signalled vector. */
#define VIRTIO_BLK_QUEUES                 1
_Static_assert(VIRTIO_BLK_MAX_REQ_SLOTS == 21,
               "64-entry queue request storage bound");
_Static_assert(sizeof(virtio_blk_req_hdr_t) * VQ_STORAGE_SIZE +
                   VQ_STORAGE_SIZE <= VIRTIO_BLK_REQUEST_DMA_BYTES,
               "request headers and status bytes fit one DMA page");
_Static_assert(VIRTIO_QUEUE_MAX_SIZE * sizeof(virtq_desc_t) +
                   sizeof(virtq_avail_t) <= PAGE_SIZE,
               "maximum legacy descriptor/available rings fit one page");
_Static_assert(sizeof(virtq_avail_t) <= PAGE_SIZE &&
                   sizeof(virtq_used_t) <= PAGE_SIZE,
               "maximum split-ring views fit one page each");

typedef struct {
    int                in_use;
    int                in_flight_counted;
    int                done;
    int                result;
    int                write;
    int                flush;   /* VIRTIO_BLK_T_FLUSH: no data descriptor */
    uint16_t           head;
    uint16_t           queue_slot;
    void              *buf;
    void              *dma_buf;
    uint64_t           dma_addr;
    uint64_t           io_dma_addr;
    size_t             bytes;
    int                direct_dma;
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
    /* Backing arrays cover the maximum queue; these negotiated counts govern
     * runtime indexing and match the Rust queue state's active bounds. */
    uint16_t           queue_size;
    uint16_t           request_slots;
    virtio_blk_req_t   req[VIRTIO_BLK_MAX_REQ_SLOTS];
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    a20_virtio_blk_queue_state_t queue_state;
#endif
    /* LOCK_ORDER: inst->lock is innermost; no nesting under or over other locks.
     * Protects req[], descriptor/avail/used rings, in_flight, status[],
     * req_hdr[], last_used, desc_idx. */
    spinlock_t         lock;
    int                slot;
    int                in_flight;
    int                irq_registered;
    /* One bit per logical CPU: set the first time a message-signalled
     * completion is handled there.  Keyed by CPU rather than by a single
     * one-shot flag so a vector that has been re-affinitized away from the
     * boot processor announces itself exactly the way the boot-CPU one does,
     * and "delivered again, on the new CPU" after the destination was
     * rewritten is the only observation that tells a moved vector from a
     * moved vector that delivers. */
    _Atomic uint32_t   msix_delivered_cpus;
} virtio_blk_inst_t;

static virtio_blk_inst_t g_insts[VIRTIO_MAX_DEVS];
static int g_ninst = 0;

static void virtio_blk_fail_queue_locked(virtio_blk_inst_t *inst,
                                         proc_wake_q_t *wake_q);

static void virtio_blk_free_dma(virtio_blk_inst_t *inst)
{
    for (int i = 0; i < VIRTIO_BLK_MAX_REQ_SLOTS; i++) {
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
 * contiguous even when their virtual addresses are contiguous.  Avoid this
 * fragile assumption by using bounded, contiguous bounce storage.  Give every in-flight slot its
 * own 64 KiB DMA buffer so concurrent filesystem requests cannot overwrite
 * each other while the device still owns a descriptor chain.
 */
static int virtio_blk_alloc_dma(virtio_blk_inst_t *inst, device_t *dev)
{
    inst->queue_dma_mem =
        dma_alloc_coherent_aligned(dev, VIRTIO_BLK_QUEUE_DMA_BYTES, PAGE_SIZE,
                                   &inst->queue_dma_addr);
    inst->request_dma_mem =
        dma_alloc_coherent_aligned(dev, VIRTIO_BLK_REQUEST_DMA_BYTES, PAGE_SIZE,
                                   &inst->request_dma_addr);
    if (!inst->queue_dma_mem || !inst->request_dma_mem)
        goto fail;

    inst->req_hdr = (virtio_blk_req_hdr_t *)inst->request_dma_mem;
    inst->status = (uint8_t *)inst->request_dma_mem +
                   sizeof(virtio_blk_req_hdr_t) * VQ_STORAGE_SIZE;
    if ((uintptr_t)(inst->status + VQ_STORAGE_SIZE) >
        (uintptr_t)inst->request_dma_mem + VIRTIO_BLK_REQUEST_DMA_BYTES)
        goto fail;

    for (int i = 0; i < VIRTIO_BLK_MAX_REQ_SLOTS; i++) {
        inst->req[i].dma_buf =
            dma_alloc_coherent_aligned(dev, VIRTIO_BLK_BOUNCE_BYTES, PAGE_SIZE,
                                       &inst->req[i].dma_addr);
        if (!inst->req[i].dma_buf)
            goto fail;
    }
    return 0;

fail:
    virtio_blk_free_dma(inst);
    return -1;
}

/* Choose the largest supported power-of-two depth in the compatibility
 * range [32, 64]. Non-power-of-two advertised maxima round down. */
static uint16_t virtio_blk_choose_queue_size(uint32_t qmax)
{
    if (qmax < VIRTIO_QUEUE_SIZE)
        return 0;

    return qmax >= VIRTIO_QUEUE_MAX_SIZE
               ? VIRTIO_QUEUE_MAX_SIZE
               : VIRTIO_QUEUE_SIZE;
}

/* Legacy MMIO places avail directly after the negotiated descriptor table,
 * not after the maximum-capacity storage view. */
static size_t virtio_blk_legacy_avail_offset(uint16_t queue_size)
{
    return (size_t)queue_size * sizeof(virtq_desc_t);
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
    uint16_t queue_size = virtio_blk_choose_queue_size(qmax);
    if (!queue_size) {
        printf("[VIRTIO%d] Queue max too small: %u (need at least %u)\n",
               idx, qmax, VIRTIO_QUEUE_SIZE);
        return -1;
    }
    /* Completed request slots remain owned by waiters across reset. Changing
     * queue geometry would invalidate their Rust queue slot handles. */
    if (inst->queue_size && inst->queue_size != queue_size) {
        printf("[VIRTIO%d] Queue depth changed across reset: %u -> %u\n",
               idx, inst->queue_size, queue_size);
        return -1;
    }

    memset(inst->queue_dma_mem, 0, VIRTIO_BLK_QUEUE_DMA_BYTES);
    memset(inst->request_dma_mem, 0, VIRTIO_BLK_REQUEST_DMA_BYTES);
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    int queue_state_rc = inst->queue_size
        ? a20_virtio_blk_queue_reset(&inst->queue_state)
        : a20_virtio_blk_queue_init(&inst->queue_state, queue_size,
                                    queue_size / 3U);
    if (queue_state_rc != 0) {
        printf("[VIRTIO%d] Rust queue state could not accept depth %u: %d\n",
               idx, queue_size, queue_state_rc);
        return -1;
    }
#endif
    inst->queue_size = queue_size;
    inst->request_slots = queue_size / 3U;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, queue_size);
    arch_dma_sync_for_device(inst->queue_dma_mem,
                             VIRTIO_BLK_QUEUE_DMA_BYTES);
    arch_dma_sync_for_device(inst->request_dma_mem,
                             VIRTIO_BLK_REQUEST_DMA_BYTES);

    if (inst->blk.legacy) {
        uint8_t *queue = (uint8_t *)inst->queue_dma_mem;
        virtq_desc_t *l_desc = (virtq_desc_t *)queue;
        virtq_avail_t *l_avail =
            (virtq_avail_t *)(queue + virtio_blk_legacy_avail_offset(
                inst->queue_size));
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

static int virtio_blk_init_instance(virtio_blk_inst_t *inst, device_t *dev) {
    int idx = inst->slot;

    inst->blk.legacy = inst->vt.legacy;
    spin_init(&inst->lock);
    if (virtio_blk_alloc_dma(inst, dev) != 0) {
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

#ifndef CONFIG_VIRTIO_BLK_CORE_RUST
static virtio_blk_req_t *virtio_blk_find_req_locked(virtio_blk_inst_t *inst, uint16_t head) {
    for (int i = 0; i < VIRTIO_BLK_MAX_REQ_SLOTS; i++) {
        virtio_blk_req_t *req = &inst->req[i];
        if (req->in_use && req->head == head)
            return req;
    }
    return NULL;
}
#endif

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
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    for (;;) {
        uint16_t ring_idx = 0;
        int ready = a20_virtio_blk_queue_used_ring_slot(
            &inst->queue_state, used_idx, &ring_idx);
        if (ready == 0)
            break;
        if (ready < 0) {
            printf("[VIRTIO%d] invalid used index %u\n", inst->slot, used_idx);
            virtio_blk_fail_queue_locked(inst, wake_q);
            return;
        }
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint32_t id = used->ring[ring_idx].id;
        uint32_t used_len = used->ring[ring_idx].len;
        uint16_t req_slot = 0;
        if (id > UINT16_MAX || a20_virtio_blk_queue_validate_head(
                &inst->queue_state, (uint16_t)id, &req_slot) != 0 ||
            req_slot >= inst->request_slots) {
            printf("[VIRTIO%d] invalid used head %u\n", inst->slot, id);
            virtio_blk_fail_queue_locked(inst, wake_q);
            return;
        }
        virtio_blk_req_t *req = &inst->req[req_slot];
        uint16_t head = req->head;
        arch_dma_sync_for_cpu(&inst->status[head], 1);
        if (!req->write)
            arch_dma_sync_for_cpu(req->direct_dma ? req->buf : req->dma_buf,
                                  req->bytes);
        uint32_t expected_len = req->write ? 1U : (uint32_t)req->bytes + 1U;
        uint16_t completed_slot = 0;
        int result = -1;
        int retired = a20_virtio_blk_queue_complete_used(
            &inst->queue_state, used_idx, (uint16_t)id, used_len,
            inst->status[head], &completed_slot, &result);
        if (retired != 1 || completed_slot != req_slot) {
            printf("[VIRTIO%d] Rust queue completion rejected head=%u\n",
                   inst->slot, id);
            virtio_blk_fail_queue_locked(inst, wake_q);
            return;
        }
        req->result = result == 0 ? 0 : -1;
        if (req->result < 0)
            printf("[VIRTIO%d] bad completion head=%u status=%u used_len=%u expected>=%u\n",
                   inst->slot, head, inst->status[head], used_len, expected_len);
        req->done = 1;
        if (wake_q)
            (void)wait_queue_collect_all(&req->waiters, 0, PROC_WAKE_EVENT,
                                         wake_q, NULL);
        inst->blk.last_used++;
        completions++;
    }
#else
    while (blk->last_used != used_idx) {
        completions++;
        uint16_t ring_idx = blk->last_used % inst->queue_size;
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t head = (uint16_t)used->ring[ring_idx].id;
        uint32_t used_len = used->ring[ring_idx].len;
        virtio_blk_req_t *req = virtio_blk_find_req_locked(inst, head);

        if (req) {
            arch_dma_sync_for_cpu(&inst->status[head], 1);
            if (!req->write)
                arch_dma_sync_for_cpu(req->direct_dma ? req->buf
                                                       : req->dma_buf,
                                      req->bytes);
            /* virtio-blk appends a 1-byte status to the device-writable buffer
             * and counts it in the descriptor length, so a read completion must
             * report bytes+1 and a write completion exactly 1.  VIRTIO_BLK_S_OK
             * is 0, so a descriptor the device never wrote keeps the 0xFF
             * sentinel set at submit time and is rejected here rather than
             * being mistaken for success. */
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
#endif
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
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (inst->in_flight > 0)
        virtio_blk_complete_used_locked(inst, &wake_q);
    spin_unlock_irqrestore(&inst->lock, flags);
    (void)proc_wake_q_flush(&wake_q);
}

void virtio_blk_poll_all(void) {
    for (int i = 0; i < g_ninst && i < VIRTIO_MAX_DEVS; i++)
        virtio_blk_poll_inst(&g_insts[i]);
}

static virtio_blk_req_t *virtio_blk_alloc_req_locked(virtio_blk_inst_t *inst,
                                                      proc_wake_q_t *wake_q,
                                                      size_t sectors, int write,
                                                      int flush) {
#ifndef CONFIG_VIRTIO_BLK_CORE_RUST
    (void)sectors;
    (void)write;
    (void)flush;
#endif
    virtio_blk_complete_used_locked(inst, wake_q);
    for (int i = 0; i < inst->request_slots; i++) {
        if (!inst->req[i].in_use) {
            virtio_blk_req_t *req = &inst->req[i];
            void *dma_buf = req->dma_buf;
            uint64_t dma_addr = req->dma_addr;
            memset(req, 0, sizeof(*req));
            req->dma_buf = dma_buf;
            req->dma_addr = dma_addr;
            wait_queue_init(&req->waiters);
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
            uint16_t queue_slot = 0, head = 0;
            uint32_t expected_len = flush ? 1U :
                (write ? 1U : (uint32_t)(sectors * VIRTIO_BLK_SECTOR_SIZE) + 1U);
            int reserve = a20_virtio_blk_queue_reserve(
                &inst->queue_state, flush ? 2 : 3, expected_len,
                write ? 1U : 0U, &queue_slot, &head);
            if (reserve != 0 || queue_slot != (uint16_t)i) {
                if (reserve == 0)
                    (void)a20_virtio_blk_queue_cancel(&inst->queue_state,
                                                       queue_slot);
                return NULL;
            }
            req->queue_slot = queue_slot;
            req->head = head;
#else
            req->in_use = 1;
            req->head = (uint16_t)(i * 3);
#endif
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
            req->in_use = 1;
#endif
            return req;
        }
    }
    return NULL;
}

static void virtio_blk_release_req_locked(virtio_blk_inst_t *inst,
                                         virtio_blk_req_t *req) {
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    int ret = a20_virtio_blk_queue_release(&inst->queue_state, req->queue_slot);
    if (ret != 0)
        printf("[VIRTIO%d] Rust request release failed slot=%u err=%d\n",
               inst->slot, req->queue_slot, ret);
#endif
    req->in_use = 0;
    if (req->in_flight_counted && inst->in_flight > 0)
        inst->in_flight--;
    req->in_flight_counted = 0;
}

static void virtio_blk_drop_in_flight_locked(virtio_blk_inst_t *inst,
                                             virtio_blk_req_t *req) {
    if (req->in_flight_counted && inst->in_flight > 0)
        inst->in_flight--;
    req->in_flight_counted = 0;
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

#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    uint16_t slots[VIRTIO_BLK_MAX_REQ_SLOTS];
    int failed = a20_virtio_blk_queue_fail_all(
        &inst->queue_state, slots, VIRTIO_BLK_MAX_REQ_SLOTS);
    if (failed < 0) {
        printf("[VIRTIO%d] Rust queue failure collection failed: %d\n",
               inst->slot, failed);
        inst->blk.valid = 0;
        return;
    }
    for (int n = 0; n < failed; n++) {
        virtio_blk_req_t *req = &inst->req[slots[n]];
        req->result = -1;
        req->done = 1;
        if (wake_q)
            (void)wait_queue_collect_all(&req->waiters, 0,
                                         PROC_WAKE_EXIT, wake_q, NULL);
    }
#else
    for (int i = 0; i < inst->request_slots; i++) {
        virtio_blk_req_t *req = &inst->req[i];
        if (!req->in_use)
            continue;
        req->result = -1;
        req->done = 1;
        if (wake_q)
            (void)wait_queue_collect_all(&req->waiters, 0,
                                         PROC_WAKE_EXIT, wake_q, NULL);
    }
#endif
    /* device_init resets the aggregate counter. Clear each request's matching
     * ownership bit first so old completed waiters cannot decrement a newer
     * request after recovery. */
    for (int i = 0; i < inst->request_slots; i++)
        inst->req[i].in_flight_counted = 0;

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

    /* One line per CPU, the first time a message-signalled completion arrives
     * there: it is the only observation that distinguishes "the capability
     * was programmed" from "the device actually posted a message and the
     * platform took it on that CPU".  It is counted before the ISR read,
     * because a device whose queue interrupt is routed through a vector need
     * not also set a shared ISR bit -- the arrival itself is the evidence. */
    if (inst->vt.msix_vectors > 0) {
        unsigned cpu = arch_current_cpu_id();
        if (cpu < 32U) {
            uint32_t bit = 1U << cpu;
            uint32_t seen = __atomic_load_n(&inst->msix_delivered_cpus,
                                            __ATOMIC_RELAXED);
            if (!(seen & bit) &&
                __atomic_compare_exchange_n(&inst->msix_delivered_cpus, &seen,
                                            seen | bit, 0,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED))
                kinfo("[VIRTIO-BLK] MSI-X delivery on vector %d cpu=%u\n",
                      irq, cpu);
        }
    }

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


static int virtio_blk_build_chain(virtq_desc_t *descs, uint16_t queue_size,
                                  uint16_t head, uint64_t header_addr,
                                  uint32_t header_len,
                                  const a20_virtio_blk_segment_t *segments,
                                  uint16_t segment_count, uint64_t status_addr,
                                  uint32_t device_writes)
{
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    return a20_virtio_blk_build_chain((a20_virtio_blk_desc_t *)descs,
        queue_size, head, header_addr, header_len, segments, segment_count,
        status_addr, device_writes);
#else
    if (!descs || queue_size < 2 || (queue_size & (queue_size - 1)) ||
        head >= queue_size || !header_len || !status_addr || device_writes > 1)
        return -EINVAL;
    uint32_t needed = (uint32_t)segment_count + 2;
    if (needed > queue_size)
        return -ENOSPC;
    if (segment_count && !segments)
        return -EINVAL;
    for (uint16_t i = 0; i < segment_count; i++)
        if (!segments[i].len)
            return -EINVAL;
    uint16_t mask = queue_size - 1;
    uint16_t slot = head;
    descs[slot] = (virtq_desc_t){ header_addr, header_len, VIRTQ_DESC_F_NEXT,
                                  (uint16_t)((slot + 1) & mask) };
    for (uint16_t i = 0; i < segment_count; i++) {
        uint16_t next = (uint16_t)((slot + 1) & mask);
        descs[next] = (virtq_desc_t){ segments[i].addr, segments[i].len,
            (uint16_t)(VIRTQ_DESC_F_NEXT |
                (device_writes ? VIRTQ_DESC_F_WRITE : 0)),
            (uint16_t)((next + 1) & mask) };
        slot = next;
    }
    uint16_t status_slot = (uint16_t)((slot + 1) & mask);
    descs[status_slot] = (virtq_desc_t){ status_addr, 1,
                                        VIRTQ_DESC_F_WRITE, 0 };
    return (int)needed;
#endif
}

static void virtio_blk_sync_desc_chain(virtio_blk_inst_t *inst, uint16_t head,
                                       uint16_t count) {
    uint16_t mask = inst->queue_size - 1;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t index = (uint16_t)((head + i) & mask);
        arch_dma_sync_for_device(&inst->blk.desc[index], sizeof(virtq_desc_t));
    }
}

static void virtio_blk_kick_locked(virtio_blk_inst_t *inst) {
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
    kernel_progress_note_pending(KERNEL_PROGRESS_PENDING_DEVICE);
}

static int virtio_blk_submit_req(virtio_blk_inst_t *inst, virtio_blk_req_t *req,
                                 uint64_t lba, void *buf, size_t sectors,
                                 int write, int flush, int notify) {
    size_t bytes = flush ? 0 : sectors * VIRTIO_BLK_SECTOR_SIZE;
    uint16_t slot = req->head;

    paddr_t pa = flush ? 0 : va_to_pa(buf);
    const pfa_range_t *range = flush ? NULL : pfa_range_for_pa(pa);
    int direct_dma = range && bytes <= range->end - pa;
    if (!flush && !direct_dma && bytes > VIRTIO_BLK_BOUNCE_BYTES) {
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
        (void)a20_virtio_blk_queue_cancel(&inst->queue_state, req->queue_slot);
#endif
        return -EINVAL;
    }

    req->done = 0;
    req->result = -1;
    req->write = write;
    req->flush = flush;
    req->buf = buf;
    req->bytes = bytes;
    inst->in_flight++;
    req->in_flight_counted = 1;

    /*
     * Kernel buffers live in A20OS's linear direct map.  If this request is
     * wholly inside one discovered RAM range, virtual contiguity therefore
     * also guarantees physical contiguity and the synchronous caller keeps
     * the storage live until completion.  Point VirtIO at that final buffer
     * directly; retain the bounded coherent bounce slot for non-RAM ranges.
     */
    req->direct_dma = direct_dma;
    req->io_dma_addr = req->direct_dma ? pa : req->dma_addr;
    void *io_buf = req->direct_dma ? buf : req->dma_buf;
    if (req->direct_dma) {
        a20_perf_count(A20_PERF_VIRTIO_BLK_DIRECT_DMAS);
    } else {
        a20_perf_count(A20_PERF_VIRTIO_BLK_BOUNCE_DMAS);
        a20_perf_add(A20_PERF_VIRTIO_BLK_BOUNCE_BYTES, bytes);
    }

    if (write && !flush && !req->direct_dma)
        memcpy(req->dma_buf, buf, bytes);

    inst->req_hdr[slot].type     = flush ? VIRTIO_BLK_T_FLUSH
                                         : (write ? VIRTIO_BLK_T_OUT
                                                  : VIRTIO_BLK_T_IN);
    inst->req_hdr[slot].reserved = 0;
    inst->req_hdr[slot].sector   = flush ? 0 : lba;

    inst->status[slot] = 0xFF;

    virtq_desc_t *desc  = inst->blk.desc;
    virtq_avail_t *avail = inst->blk.avail;

    uint64_t header_addr = inst->request_dma_addr +
                           (uint64_t)slot * sizeof(virtio_blk_req_hdr_t);
    uint64_t status_addr = inst->request_dma_addr +
                           sizeof(virtio_blk_req_hdr_t) * VQ_STORAGE_SIZE + slot;
    a20_virtio_blk_segment_t segment = { .addr = req->io_dma_addr,
                                         .len = (uint32_t)bytes };
    int desc_count = virtio_blk_build_chain(
        desc, inst->queue_size, slot,
        header_addr, sizeof(virtio_blk_req_hdr_t), flush ? NULL : &segment,
        flush ? 0 : 1, status_addr, write ? 0 : 1);
    if (desc_count < 0) {
        virtio_blk_drop_in_flight_locked(inst, req);
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
        (void)a20_virtio_blk_queue_cancel(&inst->queue_state, req->queue_slot);
#endif
        return desc_count;
    }

#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    uint16_t publish_slot = 0;
    int prepare_ret = a20_virtio_blk_queue_prepare_publish(
        &inst->queue_state, req->queue_slot, &publish_slot);
    if (prepare_ret != 0) {
        virtio_blk_drop_in_flight_locked(inst, req);
        (void)a20_virtio_blk_queue_cancel(&inst->queue_state, req->queue_slot);
        return prepare_ret;
    }
#endif

    if (flush) {
        /* Flush requests carry the header and status byte, but no data. */
        arch_dma_sync_for_device(&inst->status[slot], 1);
        virtio_blk_sync_desc_chain(inst, slot, (uint16_t)desc_count);

        uint16_t fslot =
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
            publish_slot;
#else
            avail->idx % inst->queue_size;
#endif
        avail->ring[fslot] = slot;
        arch_dma_sync_for_device(&inst->req_hdr[slot],
                                 sizeof(inst->req_hdr[slot]));
        arch_dma_sync_for_device(&avail->flags, sizeof(avail->flags));
        arch_dma_sync_for_device(&avail->ring[fslot], sizeof(uint16_t));
        wmb();
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
        uint16_t new_avail_idx = 0;
        int commit_ret = a20_virtio_blk_queue_commit_publish(
            &inst->queue_state, req->queue_slot, &new_avail_idx);
        if (commit_ret != 0) {
            (void)a20_virtio_blk_queue_cancel(&inst->queue_state,
                                              req->queue_slot);
            virtio_blk_drop_in_flight_locked(inst, req);
            return commit_ret;
        }
        avail->idx = new_avail_idx;
#else
        avail->idx++;
#endif
        inst->blk.desc_idx++;
        arch_dma_sync_for_device(&avail->idx, sizeof(avail->idx));
        wmb();
        if (notify)
            virtio_blk_kick_locked(inst);
        return 0;
    }


    /* Clean each buffer before it is published, then clean the ring slot,
     * then bump idx behind a wmb().  virtio_net.c publishes first and cleans
     * after; both are correct.  See the DMA publication contract in
     * kernel/include/drivers/dual/virtq.h before reordering either sequence. */
    uint16_t avail_slot =
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
        publish_slot;
#else
        avail->idx % inst->queue_size;
#endif
    avail->ring[avail_slot] = slot;
    arch_dma_sync_for_device(io_buf, bytes);
    arch_dma_sync_for_device(&inst->req_hdr[slot], sizeof(inst->req_hdr[slot]));
    arch_dma_sync_for_device(&inst->status[slot], 1);
    virtio_blk_sync_desc_chain(inst, slot, (uint16_t)desc_count);
    arch_dma_sync_for_device(&avail->flags, sizeof(avail->flags));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    wmb();
#ifdef CONFIG_VIRTIO_BLK_CORE_RUST
    uint16_t new_avail_idx = 0;
    int commit_ret = a20_virtio_blk_queue_commit_publish(
        &inst->queue_state, req->queue_slot, &new_avail_idx);
    if (commit_ret != 0) {
        (void)a20_virtio_blk_queue_cancel(&inst->queue_state,
                                          req->queue_slot);
        virtio_blk_drop_in_flight_locked(inst, req);
        return commit_ret;
    }
    avail->idx = new_avail_idx;
#else
    avail->idx++;
#endif
    inst->blk.desc_idx++;
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
    wmb();

    if (notify)
        virtio_blk_kick_locked(inst);
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
        if (inst->blk.valid)
            virtio_blk_complete_used_locked(inst, &wake_q);
        if (req->done) {
            int ret = req->result;
            if (ret == 0 && req->bytes && !req->write && !req->direct_dma)
                memcpy(req->buf, req->dma_buf, req->bytes);
            virtio_blk_release_req_locked(inst, req);
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            return ret;
        }
        if (timer_get_ticks() >= deadline) {
            virtio_blk_complete_used_locked(inst, &wake_q);
            if (req->done) {
                int ret = req->result;
                if (ret == 0 && req->bytes && !req->write && !req->direct_dma)
                    memcpy(req->buf, req->dma_buf, req->bytes);
                virtio_blk_release_req_locked(inst, req);
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
            uint64_t dma_addr = req->io_dma_addr;
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
         *
         * The test is whether a handler exists at all, not whether a wired
         * line was assigned: a device on message-signalled interrupts has
         * irq == -1 by construction, and parking on its vector is exactly what
         * the reservation was for.
         */
        if (!inst->irq_registered) {
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
            if (linked) {
                uint32_t perf_timed =
                    __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED);
                uint64_t park_start = perf_timed ? timer_get_ticks() : 0;
                if (perf_timed)
                    a20_perf_count(A20_PERF_VIRTIO_BLK_WAIT_PARKS);
                (void)proc_park_commit(token);
                if (perf_timed)
                    a20_perf_add(A20_PERF_VIRTIO_BLK_WAIT_PARK_TICKS,
                                 timer_get_ticks() - park_start);
            } else {
                (void)proc_park_cancel(token);
            }
            wait_queue_unlink(&req->waiters, &entry);
            proc_park_finish(token);
        } else {
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);
            cpu_relax();
        }
    }
}

static int virtio_blk_wait_req_timed(virtio_blk_inst_t *inst,
                                     virtio_blk_req_t *req, uint64_t lba) {
    uint32_t perf_timed =
        __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_RELAXED);
    uint64_t wait_start = perf_timed ? timer_get_ticks() : 0;
    if (perf_timed)
        a20_perf_count(A20_PERF_VIRTIO_BLK_WAIT_CALLS);
    int ret = virtio_blk_wait_req(inst, req, lba);
    if (perf_timed)
        a20_perf_add(A20_PERF_VIRTIO_BLK_WAIT_TICKS,
                     timer_get_ticks() - wait_start);
    return ret;
}

static int virtio_blk_rw(int idx, uint64_t lba, void *buf, size_t sectors,
                         int write, int flush) {
    if (idx < 0 || idx >= g_ninst) return -1;
    if (!flush && (!buf || sectors == 0 ||
                   sectors > VIRTIO_BLK_MAX_TRANSFER_SECTORS))
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
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            req = virtio_blk_alloc_req_locked(inst, &wake_q, sectors, write,
                                              flush);
            if (req) {
                int submit_ret = virtio_blk_submit_req(
                    inst, req, lba, buf, sectors, write, flush, 1);
                if (submit_ret < 0)
                    req->in_use = 0;
                spin_unlock_irqrestore(&inst->lock, flags);
                (void)proc_wake_q_flush(&wake_q);
                if (submit_ret < 0)
                    return submit_ret;
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

        int ret = virtio_blk_wait_req_timed(inst, req, lba);
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
        int ret = virtio_blk_rw(idx, lba, cursor, chunk, 0, 0);
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
        int ret = virtio_blk_rw(idx, lba, (void *)cursor, chunk, 1, 0);
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

static int blk_write_batch(block_dev_t *dev, const block_write_t *writes,
                           size_t nr) {
    if (nr == 0)
        return 0;
    if (!dev || !writes)
        return -EINVAL;

    for (size_t i = 0; i < nr; i++) {
        if (!writes[i].buf || writes[i].count == 0 ||
            writes[i].count > VIRTIO_BLK_MAX_TRANSFER_SECTORS ||
            writes[i].lba >= dev->capacity ||
            writes[i].count > dev->capacity - writes[i].lba)
            return -EINVAL;
    }

    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->priv;
    if (!inst || inst->slot < 0 || inst->slot >= VIRTIO_MAX_DEVS ||
        &g_insts[inst->slot] != inst || !inst->blk.valid)
        return -ENODEV;

    size_t window_limit = inst->request_slots;
    if (window_limit > 16)
        window_limit = 16;
    size_t offset = 0;

    while (offset < nr) {
        virtio_blk_req_t *pending[VIRTIO_BLK_MAX_REQ_SLOTS];
        uint64_t pending_lba[VIRTIO_BLK_MAX_REQ_SLOTS];
        size_t submitted = 0;
        int submit_error = 0;
        int queue_full = 0;

        while (submitted < window_limit && submitted < nr - offset) {
            size_t index = offset + submitted;
            proc_wake_q_t wake_q;
            proc_wake_q_init(&wake_q);
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            virtio_blk_req_t *req = NULL;
            int submit_ret = 0;
            if (!inst->blk.valid) {
                submit_error = -ENODEV;
            } else {
                req = virtio_blk_alloc_req_locked(
                    inst, &wake_q, writes[index].count, 1, 0);
                if (req) {
                    submit_ret = virtio_blk_submit_req(
                        inst, req, writes[index].lba,
                        (void *)writes[index].buf, writes[index].count,
                        1, 0, 0);
                    if (submit_ret < 0)
                        req->in_use = 0;
                } else {
                    queue_full = 1;
                }
            }
            spin_unlock_irqrestore(&inst->lock, flags);
            (void)proc_wake_q_flush(&wake_q);

            if (submit_error)
                break;
            if (!req)
                break;
            if (submit_ret < 0) {
                submit_error = submit_ret;
                break;
            }
            pending[submitted] = req;
            pending_lba[submitted] = writes[index].lba;
            submitted++;
        }

        if (submitted > 0) {
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            if (inst->blk.valid)
                virtio_blk_kick_locked(inst);
            spin_unlock_irqrestore(&inst->lock, flags);
        }

        if (submitted == 0 && queue_full && !submit_error) {
            size_t index = offset;
            int ret = blk_write_sector(dev, writes[index].lba,
                                       writes[index].buf, writes[index].count);
            if (ret < 0)
                return ret;
            offset++;
            continue;
        }

        int wait_error = 0;
        for (size_t i = 0; i < submitted; i++) {
            int ret = virtio_blk_wait_req_timed(inst, pending[i],
                                                pending_lba[i]);
            if (ret < 0 && !wait_error)
                wait_error = ret;
        }
        if (submit_error)
            return submit_error;
        if (wait_error)
            return wait_error;
        offset += submitted;
    }
    return 0;
}

int virtio_blk_flush(int idx) {
    if (idx < 0 || idx >= g_ninst) return -EINVAL;
    if (!g_insts[idx].blk.valid) return -ENODEV;
    return virtio_blk_rw(idx, 0, NULL, 0, 0, 1);
}

static int blk_flush(block_dev_t *dev) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->priv;
    return virtio_blk_flush(inst->slot);
}

block_dev_t *virtio_blk_get_dev(int idx) {
    if (idx < 0 || idx >= g_ninst) return NULL;
    if (!g_insts[idx].blk.valid) return NULL;
    g_insts[idx].blk_dev.read_sector  = blk_read_sector;
    g_insts[idx].blk_dev.write_sector = blk_write_sector;
    g_insts[idx].blk_dev.write_batch  = blk_write_batch;
    g_insts[idx].blk_dev.flush        = blk_flush;
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
        inst->vt.priv    = device_map_mmio(dev, 0);
        if (!inst->vt.priv) return -ENODEV;
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

    int ret = virtio_blk_init_instance(inst, dev);
    if (ret != 0) {
        kinfo("[VIRTIO-BLK] Init failed for device '%s'\n", dev->name);
        return ret;
    }

    /* Completion delivery, best first: one message-signalled vector per queue,
     * then the shared INTx line, then polling.  Each step is entered only when
     * the previous one failed, so a device that offers MSI-X never takes the
     * level-triggered line it was trying to get away from. */
    int msix_vectors = 0;
    if (inst->vt.msix_prepare) {
        int r = inst->vt.msix_prepare(&inst->vt, VIRTIO_BLK_QUEUES);
        if (r == 0) {
            msix_vectors = inst->vt.msix_vectors;
        } else {
            kinfo("[VIRTIO-BLK] %s: MSI-X unavailable (%d)\n", dev->name, r);
        }
    }

    if (msix_vectors > 0) {
        int registered = 0;
        for (int i = 0; i < msix_vectors; i++) {
            if (request_irq((uint32_t)(inst->vt.msix_base + i),
                            virtio_blk_irq_handler, 0, inst) == 0)
                registered++;
        }
        if (registered == msix_vectors) {
            inst->irq_registered = 1;
            /* The INTx line is no longer this device's to use: leaving a
             * handler on it would give one device two delivery paths. */
            inst->vt.irq = -1;
            inst->vt.msix_arm(&inst->vt);
            kinfo("[VIRTIO-BLK] %s using MSI-X vectors %d..%d completions\n",
                  dev->name, inst->vt.msix_base,
                  inst->vt.msix_base + msix_vectors - 1);
        } else {
            for (int i = 0; i < registered; i++)
                free_irq((uint32_t)(inst->vt.msix_base + i), inst);
            inst->vt.msix_teardown(&inst->vt);
            kinfo("[VIRTIO-BLK] %s MSI-X handler registration failed (%d/%d)\n",
                  dev->name, registered, msix_vectors);
        }
    } else if (inst->vt.irq >= 0) {
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
    if (inst->vt.irq < 0 && !inst->irq_registered)
        kinfo("[VIRTIO-BLK] %s using completion polling\n", dev->name);

    g_ninst++;
    kinfo("[VIRTIO-BLK] Probed device '%s' (legacy=%d irq=%d msix=%d)\n",
          dev->name, inst->vt.legacy, inst->vt.irq, msix_vectors);
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

static int virtio_blk_class_write_batch(struct device *dev,
                                        const struct block_write *writes,
                                        size_t nr) {
    if (!dev || !dev->drv_priv)
        return -ENODEV;
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return blk_write_batch(&inst->blk_dev, writes, nr);
}

static uint64_t virtio_blk_class_capacity(struct device *dev) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return inst->blk.capacity;
}

static uint32_t virtio_blk_class_sector_size(struct device *dev) {
    (void)dev;
    return VIRTIO_BLK_SECTOR_SIZE;
}

static int virtio_blk_class_flush(struct device *dev) {
    virtio_blk_inst_t *inst = (virtio_blk_inst_t *)dev->drv_priv;
    return virtio_blk_flush(inst->slot);
}

static block_dev_ops_t virtio_blk_class_ops = {
    .read        = virtio_blk_class_read,
    .write       = virtio_blk_class_write,
    .write_batch = virtio_blk_class_write_batch,
    .flush       = virtio_blk_class_flush,
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
        if (inst->vt.msix_vectors > 0) {
            for (int i = 0; i < inst->vt.msix_vectors; i++)
                free_irq((uint32_t)(inst->vt.msix_base + i), inst);
            inst->vt.msix_teardown(&inst->vt);
        } else {
            free_irq((uint32_t)inst->vt.irq, inst);
        }
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

static void virtio_blk_driver_shutdown(device_t *dev)
{
    (void)virtio_blk_driver_remove(dev);
}

static driver_t virtio_blk_driver = {
    .name       = "virtio-blk",
    .id_table   = virtio_blk_ids,
    .bus        = NULL,
    .probe      = virtio_blk_driver_probe,
    .remove     = virtio_blk_driver_remove,
    .shutdown   = virtio_blk_driver_shutdown,
    .progress   = virtio_blk_driver_progress,
    .class_ops  = &virtio_blk_class_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(virtio_blk_driver);
