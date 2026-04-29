#include "drv/virtio_blk.h"
#include "drv/virtio_transport.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/timer.h"
#include "proc/proc.h"

#define VQ_SIZE  VIRTIO_QUEUE_SIZE
#define VIRTIO_BLK_REQ_SLOTS (VIRTIO_QUEUE_SIZE / 3)
#define VIRTIO_BLK_WAIT_TIMEOUT_TICKS (TICKS_PER_SEC * 10)

static inline uint64_t va_to_pa(const void *va) {
    return (uint64_t)(uintptr_t)va - PAGE_OFFSET;
}

typedef struct {
    int                in_use;
    int                done;
    int                result;
    int                write;
    uint16_t           head;
    void              *buf;
    size_t             bytes;
    task_t            *waiter;
} virtio_blk_req_t;

typedef struct {
    virtio_blk_t       blk;
    block_dev_t        blk_dev;
    virtio_transport_t vt;
    virtq_desc_t       desc[VIRTIO_QUEUE_SIZE]  ALIGNED(16);
    virtq_avail_t      avail                    ALIGNED(2);
    virtq_used_t       used                     ALIGNED(4);
    ALIGNED(4096) uint8_t legacy_vq[4096 * 3];
    uint8_t            status[VIRTIO_QUEUE_SIZE];
    virtio_blk_req_hdr_t req_hdr[VIRTIO_QUEUE_SIZE];
    virtio_blk_req_t   req[VIRTIO_BLK_REQ_SLOTS];
    spinlock_t         lock;
    int                slot;
    int                in_flight;
} virtio_blk_inst_t;

static virtio_blk_inst_t g_insts[VIRTIO_MAX_DEVS];
static int g_ninst = 0;

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

    virtio_transport_t *vt = &inst->vt;
    inst->blk.valid = 0;
    inst->slot      = idx;
    spin_init(&inst->lock);
    memset(inst->req, 0, sizeof(inst->req));
    inst->blk.legacy = vt->legacy;

    printf("[VIRTIO%d] Found block device (legacy=%d)\n", idx, vt->legacy);

    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();

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

    memset(inst->desc,    0, sizeof(inst->desc));
    memset(&inst->avail,  0, sizeof(inst->avail));
    memset(&inst->used,   0, sizeof(inst->used));
    memset(inst->status,  0, sizeof(inst->status));
    memset(inst->req_hdr, 0, sizeof(inst->req_hdr));

    if (inst->blk.legacy) {
        virtq_desc_t *l_desc  = (virtq_desc_t *)(uintptr_t)inst->legacy_vq;
        virtq_avail_t *l_avail = (virtq_avail_t *)(uintptr_t)(inst->legacy_vq + VQ_SIZE * sizeof(virtq_desc_t));
        virtq_used_t  *l_used  = (virtq_used_t  *)(uintptr_t)(inst->legacy_vq + 4096);

        memcpy(l_desc, inst->desc, sizeof(inst->desc));
        memcpy(l_avail, &inst->avail, sizeof(inst->avail));
        memcpy(l_used, &inst->used, sizeof(inst->used));

        inst->blk.desc  = l_desc;
        inst->blk.avail = l_avail;
        inst->blk.used  = l_used;

        uint64_t vq_pa = va_to_pa(inst->legacy_vq);
        vt->write32(vt, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        vt->write32(vt, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_pa / 4096));
        mb();
    } else {
        uint64_t desc_pa  = va_to_pa(inst->desc);
        uint64_t avail_pa = va_to_pa(&inst->avail);
        uint64_t used_pa  = va_to_pa(&inst->used);

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

    if (!inst->blk.legacy) {
        inst->blk.desc  = inst->desc;
        inst->blk.avail = &inst->avail;
        inst->blk.used  = &inst->used;
    }
    inst->blk.desc_idx  = 0;
    inst->blk.last_used = 0;
    inst->blk.valid     = 1;

    printf("[VIRTIO%d] Block device ready: capacity=%lu sectors (%lu MB)\n",
           idx, (unsigned long)inst->blk.capacity,
           (unsigned long)(inst->blk.capacity / 2048));

    inst->blk_dev.capacity    = inst->blk.capacity;
    inst->blk_dev.sector_size = VIRTIO_BLK_SECTOR_SIZE;
    inst->blk_dev.priv        = inst;
    inst->blk_dev.read_sector = NULL;
    inst->blk_dev.write_sector = NULL;

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

static void virtio_blk_complete_used_locked(virtio_blk_inst_t *inst) {
    virtio_blk_t *blk = &inst->blk;
    virtq_used_t *used = blk->used;

    arch_dma_sync_for_cpu(&used->idx, sizeof(uint16_t));
    uint16_t used_idx = ((volatile virtq_used_t *)used)->idx;
    while (blk->last_used != used_idx) {
        uint16_t ring_idx = blk->last_used % VIRTIO_QUEUE_SIZE;
        arch_dma_sync_for_cpu(&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t head = (uint16_t)used->ring[ring_idx].id;
        virtio_blk_req_t *req = virtio_blk_find_req_locked(inst, head);

        if (req) {
            arch_dma_sync_for_cpu(&inst->status[head], 1);
            if (!req->write)
                arch_dma_sync_for_cpu(req->buf, req->bytes);
            req->result = (inst->status[head] == VIRTIO_BLK_S_OK) ? 0 : -1;
            req->done = 1;
            if (req->waiter && req->waiter->state == PROC_BLOCKED)
                proc_make_ready(req->waiter);
        }

        blk->last_used++;
    }
}

static void virtio_blk_poll_inst(virtio_blk_inst_t *inst) {
    if (!inst || !inst->blk.valid)
        return;
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (inst->in_flight > 0)
        virtio_blk_complete_used_locked(inst);
    spin_unlock_irqrestore(&inst->lock, flags);
}

void virtio_blk_poll_all(void) {
    for (int i = 0; i < g_ninst; i++)
        virtio_blk_poll_inst(&g_insts[i]);
}

static virtio_blk_req_t *virtio_blk_alloc_req_locked(virtio_blk_inst_t *inst) {
    virtio_blk_complete_used_locked(inst);
    for (int i = 0; i < VIRTIO_BLK_REQ_SLOTS; i++) {
        if (!inst->req[i].in_use) {
            virtio_blk_req_t *req = &inst->req[i];
            memset(req, 0, sizeof(*req));
            req->in_use = 1;
            req->head = (uint16_t)(i * 3);
            return req;
        }
    }
    return NULL;
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
    req->waiter = proc_current();
    inst->in_flight++;

    inst->req_hdr[slot].type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    inst->req_hdr[slot].reserved = 0;
    inst->req_hdr[slot].sector   = lba;

    inst->status[slot] = 0xFF;

    virtq_desc_t *desc  = inst->blk.desc;
    virtq_avail_t *avail = inst->blk.avail;

    desc[slot].addr  = va_to_pa(&inst->req_hdr[slot]);
    desc[slot].len   = sizeof(virtio_blk_req_hdr_t);
    desc[slot].flags = VIRTQ_DESC_F_NEXT;
    desc[slot].next  = slot + 1;

    desc[slot + 1].addr  = va_to_pa(buf);
    desc[slot + 1].len   = (uint32_t)bytes;
    desc[slot + 1].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    desc[slot + 1].next  = slot + 2;

    desc[slot + 2].addr  = va_to_pa(&inst->status[slot]);
    desc[slot + 2].len   = 1;
    desc[slot + 2].flags = VIRTQ_DESC_F_WRITE;
    desc[slot + 2].next  = 0;

    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = slot;
    wmb();
    avail->idx++;
    inst->blk.desc_idx++;
    wmb();

    arch_dma_sync_for_device(buf, bytes);
    arch_dma_sync_for_device(&inst->req_hdr[slot], sizeof(inst->req_hdr[slot]));
    arch_dma_sync_for_device(&desc[slot], sizeof(virtq_desc_t) * 3);
    arch_dma_sync_for_device(&avail->flags, sizeof(avail->flags));
    arch_dma_sync_for_device(&avail->ring[avail_slot], sizeof(uint16_t));
    arch_dma_sync_for_device(&avail->idx, sizeof(uint16_t));
    arch_dma_sync_for_device(&inst->blk.used->idx, sizeof(uint16_t));
    arch_dma_sync_for_device(&inst->status[slot], 1);

    vt->write32(vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
    return 0;
}

static int virtio_blk_wait_req(virtio_blk_inst_t *inst, virtio_blk_req_t *req,
                               uint64_t lba) {
    task_t *cur = proc_current();
    uint64_t deadline = timer_get_ticks() + VIRTIO_BLK_WAIT_TIMEOUT_TICKS;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&inst->lock);
        virtio_blk_complete_used_locked(inst);
        if (req->done) {
            int ret = req->result;
            req->in_use = 0;
            if (inst->in_flight > 0)
                inst->in_flight--;
            spin_unlock_irqrestore(&inst->lock, flags);
            return ret;
        }
        spin_unlock_irqrestore(&inst->lock, flags);

        if (timer_get_ticks() >= deadline) {
            uint32_t dev_status = inst->vt.read32(&inst->vt, VIRTIO_MMIO_STATUS);
            printf("[VIRTIO%d] I/O timeout! lba=%lu dev_status=0x%x\n",
                   inst->slot, (unsigned long)lba, dev_status);
            flags = spin_lock_irqsave(&inst->lock);
            if (!req->done) {
                req->done = 1;
                req->result = -1;
            }
            req->in_use = 0;
            if (inst->in_flight > 0)
                inst->in_flight--;
            spin_unlock_irqrestore(&inst->lock, flags);
            return -1;
        }

        if (cur) {
            proc_set_wake_time(cur, deadline);
            cur->state = PROC_BLOCKED;
            virtio_blk_poll_inst(inst);
            sched();
            proc_set_wake_time(cur, 0);
        } else {
            __asm__ volatile("nop");
        }
    }
}

static int virtio_blk_rw(int idx, uint64_t lba, void *buf, size_t sectors, int write) {
    if (idx < 0 || idx >= g_ninst) return -1;
    virtio_blk_inst_t *inst = &g_insts[idx];
    if (!inst->blk.valid) return -1;

    virtio_blk_req_t *req = NULL;
    while (!req) {
        uint64_t flags = spin_lock_irqsave(&inst->lock);
        req = virtio_blk_alloc_req_locked(inst);
        if (req) {
            virtio_blk_submit_req(inst, req, lba, buf, sectors, write);
            spin_unlock_irqrestore(&inst->lock, flags);
            break;
        }
        spin_unlock_irqrestore(&inst->lock, flags);
        if (proc_current())
            proc_yield();
        else
            __asm__ volatile("nop");
    }

    int ret = virtio_blk_wait_req(inst, req, lba);
    if (ret < 0) {
        uint16_t head = req ? req->head : 0;
        printf("[VIRTIO%d] I/O error: status=%d lba=%lu\n",
               idx, inst->status[head], (unsigned long)lba);
    }
    return ret;
}

int virtio_blk_read(int idx, uint64_t lba, void *buf, size_t sectors) {
    return virtio_blk_rw(idx, lba, buf, sectors, 0);
}

int virtio_blk_write(int idx, uint64_t lba, const void *buf, size_t sectors) {
    return virtio_blk_rw(idx, lba, (void *)buf, sectors, 1);
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
