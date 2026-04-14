#include "virtio_blk.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "defs.h"
#include "consts.h"

#define VIRTIO0_BASE    0x10001000UL

#define VQ_SIZE  VIRTIO_QUEUE_SIZE

typedef struct {
    virtio_blk_t       blk;
    block_dev_t        blk_dev;
    virtq_desc_t       desc[VIRTIO_QUEUE_SIZE]  ALIGNED(16);
    virtq_avail_t      avail                    ALIGNED(2);
    virtq_used_t       used                     ALIGNED(4);
    ALIGNED(4096) uint8_t legacy_vq[4096 * 3];
    uint8_t            status[VIRTIO_QUEUE_SIZE];
    virtio_blk_req_hdr_t req_hdr[VIRTIO_QUEUE_SIZE];
    int                slot;
} virtio_blk_inst_t;

static virtio_blk_inst_t g_insts[VIRTIO_MAX_DEVS];
static int g_ninst = 0;

static uint32_t mmio_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void mmio_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

int virtio_blk_init(uintptr_t mmio_base) {
    if (g_ninst >= VIRTIO_MAX_DEVS) {
        printf("[VIRTIO] Too many devices (max %d)\n", VIRTIO_MAX_DEVS);
        return -1;
    }

    int idx = g_ninst;
    virtio_blk_inst_t *inst = &g_insts[idx];
    uintptr_t base = mmio_base ? mmio_base : (VIRTIO0_BASE + (unsigned long)idx * 0x1000);
    volatile uint32_t *regs = (volatile uint32_t *)base;

    inst->blk.base  = regs;
    inst->blk.valid = 0;
    inst->slot      = idx;

    uint32_t magic   = mmio_read32(base, VIRTIO_MMIO_MAGIC);
    uint32_t version = mmio_read32(base, VIRTIO_MMIO_VERSION);
    uint32_t dev_id  = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);

    if (magic != 0x74726976) {
        printf("[VIRTIO%d] Bad magic: 0x%x\n", idx, magic);
        return -1;
    }
    if (version != 1 && version != 2) {
        printf("[VIRTIO%d] Unsupported version: %d\n", idx, version);
        return -1;
    }
    inst->blk.legacy = (version == 1);
    if (dev_id != 2) {
        printf("[VIRTIO%d] Not a block device: dev_id=%d\n", idx, dev_id);
        return -1;
    }

    printf("[VIRTIO%d] Found block device at 0x%lx (version %d)\n",
           idx, (unsigned long)base, version);

    mmio_write32(base, VIRTIO_MMIO_STATUS, 0);
    mb();

    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    mb();

    mmio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    mmio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);

    /* Negotiate feature bits 32-63 — MUST accept VIRTIO_F_VERSION_1 (bit 32)
     * for non-legacy (v2) devices, otherwise QEMU's virtio core may silently
     * reject requests or behave unpredictably. */
    mmio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = mmio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t driver_hi = 0;
    if (!inst->blk.legacy) {
        driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    }
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();

    if (!inst->blk.legacy) {
        status |= VIRTIO_STATUS_FEATURES_OK;
        mmio_write32(base, VIRTIO_MMIO_STATUS, status);
        mb();
        uint32_t s = mmio_read32(base, VIRTIO_MMIO_STATUS);
        if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
            printf("[VIRTIO%d] Device rejected features (lo=0x%x hi=0x%x)\n",
                   idx, 0, driver_hi);
            return -1;
        }
    }

    mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < VIRTIO_QUEUE_SIZE) {
        printf("[VIRTIO%d] Queue max too small: %d\n", idx, qmax);
        return -1;
    }
    mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);

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

        uint64_t vq_pa = (uint64_t)(uintptr_t)inst->legacy_vq;
        mmio_write32(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        mmio_write32(base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_pa / 4096));
        mb();
    } else {
        uint64_t desc_pa  = (uint64_t)(uintptr_t)inst->desc;
        uint64_t avail_pa = (uint64_t)(uintptr_t)&inst->avail;
        uint64_t used_pa  = (uint64_t)(uintptr_t)&inst->used;

        mmio_write32(base, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)(desc_pa));
        mmio_write32(base, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_pa  >> 32));
        mmio_write32(base, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)(avail_pa));
        mmio_write32(base, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_pa >> 32));
        mmio_write32(base, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)(used_pa));
        mmio_write32(base, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_pa  >> 32));
        mb();
        mmio_write32(base, VIRTIO_MMIO_QUEUE_READY, 1);
        mb();
    }

    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    mb();

    uint64_t cap_lo = mmio_read32(base, VIRTIO_MMIO_CONFIG + 0);
    uint64_t cap_hi = mmio_read32(base, VIRTIO_MMIO_CONFIG + 4);
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

static int virtio_blk_rw(int idx, uint64_t lba, void *buf, size_t sectors, int write) {
    if (idx < 0 || idx >= g_ninst) return -1;
    virtio_blk_inst_t *inst = &g_insts[idx];
    if (!inst->blk.valid) return -1;

    size_t bytes = sectors * VIRTIO_BLK_SECTOR_SIZE;

    inst->req_hdr[0].type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    inst->req_hdr[0].reserved = 0;
    inst->req_hdr[0].sector   = lba;

    inst->status[0] = 0xFF;

    virtq_desc_t *desc  = inst->blk.desc;
    virtq_avail_t *avail = inst->blk.avail;
    virtq_used_t  *used  = inst->blk.used;

    desc[0].addr  = (uint64_t)(uintptr_t)&inst->req_hdr[0];
    desc[0].len   = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = (uint64_t)(uintptr_t)buf;
    desc[1].len   = (uint32_t)bytes;
    desc[1].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    desc[1].next  = 2;

    desc[2].addr  = (uint64_t)(uintptr_t)&inst->status[0];
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = 0;
    wmb();
    avail->idx++;
    wmb();

    mmio_write32((uintptr_t)inst->blk.base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    uint32_t timeout = 10000000;
    while (used->idx == inst->blk.last_used && timeout > 0) {
        timeout--;
        __asm__ volatile("nop");
    }

    if (timeout == 0) {
        uint32_t dev_status = mmio_read32((uintptr_t)inst->blk.base, VIRTIO_MMIO_STATUS);
        printf("[VIRTIO%d] I/O timeout! lba=%lu dev_status=0x%x\n",
               idx, (unsigned long)lba, dev_status);
        return -1;
    }

    rmb();
    inst->blk.last_used = used->idx;

    if (inst->status[0] != VIRTIO_BLK_S_OK) {
        printf("[VIRTIO%d] I/O error: status=%d lba=%lu\n",
               idx, inst->status[0], (unsigned long)lba);
        return -1;
    }

    return 0;
}

int virtio_blk_read(int idx, uint64_t lba, void *buf, size_t sectors) {
    for (size_t i = 0; i < sectors; i++) {
        int r = virtio_blk_rw(idx, lba + i, (char *)buf + i * VIRTIO_BLK_SECTOR_SIZE, 1, 0);
        if (r < 0) return r;
    }
    return 0;
}

int virtio_blk_write(int idx, uint64_t lba, const void *buf, size_t sectors) {
    for (size_t i = 0; i < sectors; i++) {
        int r = virtio_blk_rw(idx, lba + i, (char *)buf + i * VIRTIO_BLK_SECTOR_SIZE, 1, 1);
        if (r < 0) return r;
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
