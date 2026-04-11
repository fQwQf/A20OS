/*
 * A20OS — virtio-blk MMIO driver
 *
 * Implements VirtIO 1.1 block device over MMIO transport.
 * RISC-V QEMU virt machine maps the first virtio device at 0x10001000.
 * LoongArch uses PCI-based virtio (handled separately).
 *
 * Design inspired by RocketOS arch/riscv64/virtio_blk.rs
 * and xv6-riscv kernel/virtio_disk.c
 */

#include "virtio_blk.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "defs.h"
#include "consts.h"

/* RISC-V QEMU virt: first virtio MMIO device */
#define VIRTIO0_BASE    0x10001000UL

static virtio_blk_t  g_blk;
static block_dev_t   g_blk_dev;

#define VQ_SIZE  VIRTIO_QUEUE_SIZE

static virtq_desc_t  g_desc[VIRTIO_QUEUE_SIZE]   ALIGNED(16);
static virtq_avail_t g_avail                      ALIGNED(2);
static virtq_used_t  g_used                       ALIGNED(4);

static ALIGNED(4096) uint8_t g_legacy_vq[4096 * 3];

/* Per-request status byte buffer (one per descriptor slot) */
static uint8_t g_status[VIRTIO_QUEUE_SIZE];

/* Request headers */
static virtio_blk_req_hdr_t g_req_hdr[VIRTIO_QUEUE_SIZE];

/* ---- MMIO helpers ---- */

static uint32_t mmio_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void mmio_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

/* ---- Initialize the virtio-blk device ---- */

int virtio_blk_init(uintptr_t mmio_base) {
    uintptr_t base = mmio_base ? mmio_base : VIRTIO0_BASE;
    volatile uint32_t *regs = (volatile uint32_t *)base;

    g_blk.base  = regs;
    g_blk.valid = 0;

    /* Validate magic and version */
    uint32_t magic   = mmio_read32(base, VIRTIO_MMIO_MAGIC);
    uint32_t version = mmio_read32(base, VIRTIO_MMIO_VERSION);
    uint32_t dev_id  = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);

    if (magic != 0x74726976) {
        printf("[VIRTIO] Bad magic: 0x%x\n", magic);
        return -1;
    }
    if (version != 1 && version != 2) {
        printf("[VIRTIO] Unsupported version: %d\n", version);
        return -1;
    }
    g_blk.legacy = (version == 1);
    if (dev_id != 2) { /* 2 = block device */
        printf("[VIRTIO] Not a block device: dev_id=%d\n", dev_id);
        return -1;
    }

    printf("[VIRTIO] Found block device at 0x%lx (version %d)\n", (unsigned long)base, version);

    /* Reset device */
    mmio_write32(base, VIRTIO_MMIO_STATUS, 0);
    mb();

    /* Set ACKNOWLEDGE + DRIVER */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    mb();

    /* Negotiate features — we want minimal (no extra features) */
    mmio_write32(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features = mmio_read32(base, VIRTIO_MMIO_DEVICE_FEATURES);
    features &= 0x0;
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(base, VIRTIO_MMIO_DRIVER_FEATURES, features);
    mb();

    if (!g_blk.legacy) {
        status |= VIRTIO_STATUS_FEATURES_OK;
        mmio_write32(base, VIRTIO_MMIO_STATUS, status);
        mb();

        uint32_t s = mmio_read32(base, VIRTIO_MMIO_STATUS);
        if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
            printf("[VIRTIO] Device rejected features\n");
            return -1;
        }
    }

    /* Configure virtqueue 0 */
    mmio_write32(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = mmio_read32(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || qmax < VIRTIO_QUEUE_SIZE) {
        printf("[VIRTIO] Queue max too small: %d\n", qmax);
        return -1;
    }
    mmio_write32(base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);

    memset(g_desc,   0, sizeof(g_desc));
    memset(&g_avail, 0, sizeof(g_avail));
    memset(&g_used,  0, sizeof(g_used));
    memset(g_status, 0, sizeof(g_status));
    memset(g_req_hdr, 0, sizeof(g_req_hdr));

    if (g_blk.legacy) {
        virtq_desc_t *l_desc  = (virtq_desc_t *)(uintptr_t)g_legacy_vq;
        virtq_avail_t *l_avail = (virtq_avail_t *)(uintptr_t)(g_legacy_vq + VQ_SIZE * sizeof(virtq_desc_t));
        virtq_used_t  *l_used  = (virtq_used_t  *)(uintptr_t)(g_legacy_vq + 4096);

        memcpy(l_desc, g_desc, sizeof(g_desc));
        memcpy(l_avail, &g_avail, sizeof(g_avail));
        memcpy(l_used, &g_used, sizeof(g_used));

        g_blk.desc  = l_desc;
        g_blk.avail = l_avail;
        g_blk.used  = l_used;

        uint64_t vq_pa = (uint64_t)(uintptr_t)g_legacy_vq;

        mmio_write32(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
        mb();
        mmio_write32(base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(vq_pa / 4096));
        mb();
    } else {
        uint64_t desc_pa  = (uint64_t)(uintptr_t)g_desc;
        uint64_t avail_pa = (uint64_t)(uintptr_t)&g_avail;
        uint64_t used_pa  = (uint64_t)(uintptr_t)&g_used;

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

    /* Set DRIVER_OK */
    status |= VIRTIO_STATUS_DRIVER_OK;
    mmio_write32(base, VIRTIO_MMIO_STATUS, status);
    mb();

    /* Read capacity from config space */
    /* Config space starts at VIRTIO_MMIO_CONFIG (0x100) */
    /* VirtIO blk config: first 8 bytes = capacity (little-endian) */
    uint64_t cap_lo = mmio_read32(base, VIRTIO_MMIO_CONFIG + 0);
    uint64_t cap_hi = mmio_read32(base, VIRTIO_MMIO_CONFIG + 4);
    g_blk.capacity = cap_lo | (cap_hi << 32);

    if (!g_blk.legacy) {
        g_blk.desc      = g_desc;
        g_blk.avail     = &g_avail;
        g_blk.used      = &g_used;
    }
    g_blk.desc_idx  = 0;
    g_blk.last_used = 0;
    g_blk.valid     = 1;

    printf("[VIRTIO] Block device ready: capacity=%lu sectors (%lu MB)\n",
           (unsigned long)g_blk.capacity,
           (unsigned long)(g_blk.capacity / 2048));

    /* Set up abstract block_dev interface */
    g_blk_dev.read_sector  = NULL; /* set below */
    g_blk_dev.write_sector = NULL;
    g_blk_dev.capacity     = g_blk.capacity;
    g_blk_dev.sector_size  = VIRTIO_BLK_SECTOR_SIZE;
    g_blk_dev.priv         = &g_blk;

    return 0;
}

/* ---- Submit an I/O request (blocking poll) ---- */

static int virtio_blk_rw(uint64_t lba, void *buf, size_t sectors, int write) {
    if (!g_blk.valid) return -1;

    uintptr_t base = (uintptr_t)g_blk.base;
    size_t bytes = sectors * VIRTIO_BLK_SECTOR_SIZE;

    /* Use descriptor chain: [0]=header, [1]=data, [2]=status */
    int d0 = 0, d1 = 1, d2 = 2;
    (void)d0; (void)d1; (void)d2;

    virtq_desc_t *desc  = g_blk.desc;
    virtq_avail_t *avail = g_blk.avail;
    virtq_used_t  *used  = g_blk.used;

    g_req_hdr[0].type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    g_req_hdr[0].reserved = 0;
    g_req_hdr[0].sector   = lba;

    g_status[0] = 0xFF;

    desc[0].addr  = (uint64_t)(uintptr_t)&g_req_hdr[0];
    desc[0].len   = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = (uint64_t)(uintptr_t)buf;
    desc[1].len   = (uint32_t)bytes;
    desc[1].flags = (write ? 0 : VIRTQ_DESC_F_WRITE) | VIRTQ_DESC_F_NEXT;
    desc[1].next  = 2;

    desc[2].addr  = (uint64_t)(uintptr_t)&g_status[0];
    desc[2].len   = 1;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next  = 0;

    uint16_t avail_slot = avail->idx % VIRTIO_QUEUE_SIZE;
    avail->ring[avail_slot] = 0;
    wmb();
    avail->idx++;
    wmb();

    mmio_write32((uintptr_t)g_blk.base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();

    uint32_t timeout = 10000000;
    while (used->idx == g_blk.last_used && timeout > 0) {
        timeout--;
        __asm__ volatile("nop");
    }

    if (timeout == 0) {
        printf("[VIRTIO] I/O timeout!\n");
        return -1;
    }

    g_blk.last_used = used->idx;

    /* Check status */
    if (g_status[0] != VIRTIO_BLK_S_OK) {
        printf("[VIRTIO] I/O error: status=%d\n", g_status[0]);
        return -1;
    }

    return 0;
}

int virtio_blk_read(uint64_t lba, void *buf, size_t sectors) {
    /* Read one sector at a time for simplicity */
    for (size_t i = 0; i < sectors; i++) {
        int r = virtio_blk_rw(lba + i, (char *)buf + i * VIRTIO_BLK_SECTOR_SIZE, 1, 0);
        if (r < 0) return r;
    }
    return 0;
}

int virtio_blk_write(uint64_t lba, const void *buf, size_t sectors) {
    for (size_t i = 0; i < sectors; i++) {
        int r = virtio_blk_rw(lba + i, (char *)buf + i * VIRTIO_BLK_SECTOR_SIZE, 1, 1);
        if (r < 0) return r;
    }
    return 0;
}

uint64_t virtio_blk_capacity(void) {
    return g_blk.capacity;
}

int virtio_blk_ready(void) {
    return g_blk.valid;
}

/* Internal block_dev callbacks */
static int blk_read_sector(block_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    (void)dev;
    return virtio_blk_read(lba, buf, count);
}

static int blk_write_sector(block_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    (void)dev;
    return virtio_blk_write(lba, buf, count);
}

block_dev_t *virtio_blk_get_dev(void) {
    if (!g_blk.valid) return NULL;
    g_blk_dev.read_sector  = blk_read_sector;
    g_blk_dev.write_sector = blk_write_sector;
    return &g_blk_dev;
}
