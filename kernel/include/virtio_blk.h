#ifndef _VIRTIO_BLK_H
#define _VIRTIO_BLK_H

#include "types.h"

/* ============================================================
 * VirtIO Block Device Driver — MMIO transport
 * Supports both RISC-V (bus=virtio-mmio-bus) and LoongArch
 * (PCI/PCIe — detected via DTB, transparent to upper layers)
 *
 * References:
 *   • VirtIO spec v1.1 §5.2 (Block Device)
 *   • QEMU hw/virtio/virtio-mmio.c
 *   • RocketOS arch/riscv64/virtio_blk.rs
 * ============================================================ */

/* MMIO register offsets (VirtIO MMIO v2) */
#define VIRTIO_MMIO_MAGIC           0x000   /* 0x74726976 */
#define VIRTIO_MMIO_VERSION         0x004   /* must be 2 */
#define VIRTIO_MMIO_DEVICE_ID       0x008   /* 2 = block */
#define VIRTIO_MMIO_VENDOR_ID       0x00C
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_READY     0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0A0
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0A4
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0FC
#define VIRTIO_MMIO_CONFIG          0x100

#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
#define VIRTIO_MMIO_QUEUE_PFN       0x040

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096UL
#endif

/* VirtIO device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED        0x80

/* VirtIO block request types */
#define VIRTIO_BLK_T_IN          0   /* read */
#define VIRTIO_BLK_T_OUT         1   /* write */
#define VIRTIO_BLK_T_FLUSH       4   /* flush */
#define VIRTIO_BLK_T_GET_ID      8   /* get ID string */

/* VirtIO block status */
#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

/* VirtIO descriptor flags */
#define VIRTQ_DESC_F_NEXT        1
#define VIRTQ_DESC_F_WRITE       2   /* device writes (read from guest perspective) */
#define VIRTQ_DESC_F_INDIRECT    4

/* Queue size (must be power of 2) */
#define VIRTIO_QUEUE_SIZE        8

/* Sector size */
#define VIRTIO_BLK_SECTOR_SIZE   512

/* ---- VirtIO Virtqueue structures ---- */

/* VirtIO descriptor */
typedef struct {
    uint64_t addr;      /* physical address */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

/* VirtIO available ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t used_event;
} __attribute__((packed)) virtq_avail_t;

/* VirtIO used ring element */
typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

/* VirtIO used ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) virtq_used_t;

/* VirtIO block request header */
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

/* ---- Driver state ---- */
typedef struct virtio_blk {
    volatile uint32_t *base;        /* MMIO base address */
    virtq_desc_t      *desc;        /* descriptor table */
    virtq_avail_t     *avail;       /* available ring */
    virtq_used_t      *used;        /* used ring */
    uint16_t           desc_idx;    /* next free descriptor */
    uint16_t           last_used;   /* last seen used index */
    uint64_t           capacity;    /* sectors */
    int                valid;       /* initialized? */
    int                legacy;      /* virtio legacy (version 1)? */
} virtio_blk_t;

/* ============================================================
 * Block device abstract interface
 * Allows FAT32/VFS layer to use any block device
 * ============================================================ */
typedef struct block_dev {
    int    (*read_sector)(struct block_dev *dev, uint64_t lba, void *buf, size_t count);
    int    (*write_sector)(struct block_dev *dev, uint64_t lba, const void *buf, size_t count);
    uint64_t capacity;      /* total sectors */
    uint32_t sector_size;
    void    *priv;          /* driver-specific data */
} block_dev_t;

/* ============================================================
 * Public API
 * ============================================================ */

/* Initialize virtio-blk at given MMIO base address */
int  virtio_blk_init(uintptr_t mmio_base);

/* Read/write N sectors starting at LBA */
int  virtio_blk_read(uint64_t lba, void *buf, size_t sectors);
int  virtio_blk_write(uint64_t lba, const void *buf, size_t sectors);

/* Total sector count */
uint64_t virtio_blk_capacity(void);

/* Get block_dev pointer for VFS/FAT32 use */
block_dev_t *virtio_blk_get_dev(void);

/* Check if device is ready */
int virtio_blk_ready(void);

#endif /* _VIRTIO_BLK_H */
