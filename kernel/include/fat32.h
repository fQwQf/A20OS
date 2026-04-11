#ifndef _FAT32_H
#define _FAT32_H

#include "types.h"
#include "vfs.h"
#include "virtio_blk.h"

/* ============================================================
 * FAT32 Filesystem Driver
 * Supports creation, reading, writing, seeking, directory
 * listing (long filename support via VFAT LFN).
 * ============================================================ */

/* FAT32 constants */
#define FAT32_SECTOR_SIZE       512
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_BAD       0x0FFFFFF7
#define FAT32_CLUSTER_END       0x0FFFFFF8   /* >= this = end-of-chain */
#define FAT32_CLUSTER_END_MARK  0x0FFFFFFF

/* Directory entry attribute bytes */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOL_LABEL  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F  /* all 4 low bits set = LFN entry */

/* ---- BIOS Parameter Block (BPB) — first 90 bytes of boot sector ---- */
typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;    /* 0 if > 65535 */
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];          /* "FAT32   " */
} fat32_bpb_t;

/* ---- Regular 8.3 directory entry (32 bytes) ---- */
typedef struct __attribute__((packed)) {
    uint8_t  name[11];            /* 8.3 format, space-padded */
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;         /* high 16 bits of first cluster */
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;         /* low 16 bits of first cluster */
    uint32_t file_size;
} fat32_dirent_t;

/* ---- LFN directory entry (32 bytes) ---- */
typedef struct __attribute__((packed)) {
    uint8_t  order;               /* seq number (1-based), 0x40 = last */
    uint16_t name1[5];            /* chars 1-5 */
    uint8_t  attr;                /* must be FAT_ATTR_LFN */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];            /* chars 6-11 */
    uint16_t fst_clus;            /* must be 0 */
    uint16_t name3[2];            /* chars 12-13 */
} fat32_lfn_t;

/* ---- In-memory FAT32 superblock info ---- */
typedef struct fat32_sb {
    uint32_t first_fat_sector;
    uint32_t sectors_per_fat;
    uint32_t first_data_sector;
    uint32_t root_cluster;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t total_clusters;
    block_dev_t *dev;
} fat32_sb_t;

/* ---- Per-file/dir handle ---- */
typedef struct fat32_file {
    fat32_sb_t *sb;
    uint32_t   first_cluster;
    uint32_t   cur_cluster;
    size_t     size;
    size_t     offset;
    int        is_dir;
    /* For directory iteration */
    uint32_t   dir_cluster;
    size_t     dir_offset;     /* byte offset within directory cluster chain */
} fat32_file_t;

/* ============================================================
 * FAT32 VFS integration
 * Returns a vfs_ops_t / vnode_ops_t pair for mounting
 * ============================================================ */

/* Mount FAT32 on the given block device.
 * Returns a vnode_t* representing the root directory, or NULL on error. */
vnode_t *fat32_mount(block_dev_t *dev);

/* Called by VFS to free resources on unmount */
void fat32_unmount(vnode_t *root);

#endif /* _FAT32_H */
