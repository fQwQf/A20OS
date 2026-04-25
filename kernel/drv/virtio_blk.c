#include "virtio_blk.h"
#include "virtio_transport.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "defs.h"
#include "consts.h"

#define VQ_SIZE  VIRTIO_QUEUE_SIZE

static inline uint64_t va_to_pa(const void *va) {
    return (uint64_t)(uintptr_t)va - PAGE_OFFSET;
}

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
    int                slot;
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

static int virtio_blk_rw(int idx, uint64_t lba, void *buf, size_t sectors, int write) {
    if (idx < 0 || idx >= g_ninst) return -1;
    virtio_blk_inst_t *inst = &g_insts[idx];
    if (!inst->blk.valid) return -1;

    size_t bytes = sectors * VIRTIO_BLK_SECTOR_SIZE;
    virtio_transport_t *vt = &inst->vt;

    uint16_t slot = inst->blk.desc_idx % VIRTIO_QUEUE_SIZE;
    if (slot + 2 >= VIRTIO_QUEUE_SIZE)
        slot = 0;

    inst->req_hdr[slot].type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    inst->req_hdr[slot].reserved = 0;
    inst->req_hdr[slot].sector   = lba;

    inst->status[slot] = 0xFF;

    virtq_desc_t *desc  = inst->blk.desc;
    virtq_avail_t *avail = inst->blk.avail;
    virtq_used_t  *used  = inst->blk.used;

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

#ifdef CONFIG_LOONGARCH64
    // 在通知 VirtIO 去搬运数据之前，必须先处理 Cache 一致性
    // 无论是读（强制后续从内存读新数据）还是写（强制脏数据写回物理内存），
    // 统一执行 Flush 操作即可保证安全。
    arch_dcache_flush((uintptr_t)buf, bytes);
    // 请求头里包含 type/sector；如果这里仍是旧 cache line，
    // 设备就会访问错误的 LBA，表现为复制后的 ELF 内容随机损坏。
    arch_dcache_flush((uintptr_t)&inst->req_hdr[slot], sizeof(inst->req_hdr[slot]));
    // 同时，VirtIO 的描述符表 (desc, avail) 本身也可能停留在 Cache 中，
    // 为了极致稳妥，可以将当前的 ring 状态也刷回内存：
    arch_dcache_flush((uintptr_t)&desc[slot], sizeof(virtq_desc_t) * 3);
    arch_dcache_flush((uintptr_t)&avail->flags, sizeof(avail->flags));
    arch_dcache_flush((uintptr_t)&avail->ring[avail_slot], sizeof(uint16_t));
    arch_dcache_flush((uintptr_t)&avail->idx, sizeof(uint16_t));
    arch_dcache_flush((uintptr_t)&used->idx, sizeof(uint16_t));
    arch_dcache_flush((uintptr_t)&inst->status[slot], 1);
#endif

    vt->write32(vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    uint32_t timeout = 0x0FFFFFFF;
    while (timeout > 0) {
#ifdef CONFIG_LOONGARCH64
        arch_dcache_flush((uintptr_t)&used->idx, sizeof(uint16_t));
        arch_dcache_flush((uintptr_t)&inst->status[slot], 1);
#endif
        if (((volatile virtq_used_t *)used)->idx != inst->blk.last_used)
            break;
        timeout--;
        __asm__ volatile("nop");
    }

    if (timeout == 0) {
        uint32_t dev_status = vt->read32(vt, VIRTIO_MMIO_STATUS);
        printf("[VIRTIO%d] I/O timeout! lba=%lu dev_status=0x%x\n",
               idx, (unsigned long)lba, dev_status);
        return -1;
    }

    rmb();
#ifdef CONFIG_LOONGARCH64
    arch_dcache_flush((uintptr_t)&used->idx, sizeof(uint16_t));
    arch_dcache_flush((uintptr_t)&inst->status[slot], 1);
    if (!write)
        arch_dcache_flush((uintptr_t)buf, bytes);
#endif
    inst->blk.last_used = used->idx;

    if (inst->status[slot] != VIRTIO_BLK_S_OK) {
        printf("[VIRTIO%d] I/O error: status=%d lba=%lu\n",
               idx, inst->status[slot], (unsigned long)lba);
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
