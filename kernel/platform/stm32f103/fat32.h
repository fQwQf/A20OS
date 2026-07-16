#ifndef _STM32F103_FAT32_H
#define _STM32F103_FAT32_H

/*
 * Compact FAT32 file layer (hardware-independent).
 *
 * The onboard Flash is tiny, so backgrounds, Live2D sprite frames, the CJK
 * font, config and logs all live on the TF card. sdcard.c already brings the
 * card up and gives sector read/write + the partition LBA; this module adds
 * real *file* read/write on top of that sector interface: mount, open, read,
 * seek, create/truncate, sequential write, mkdir, unlink.
 *
 * It talks to storage only through a fat32_io_t callback pair, so the exact
 * same code runs on the STM32 (backed by stm32_sdcard_read/write) and in the
 * host test / simulator (backed by a real FAT32 disk image file). Names are
 * matched on their 8.3 short form (LFN entries are skipped); files this module
 * creates use 8.3-uppercase names, which is what the hub's assets use.
 *
 * Single-threaded use only (one shared metadata scratch sector per fs).
 */

#include "core/types.h"

#define FAT32_SECTOR_SIZE 512u
#define FAT32_PATH_MAX    128u

/* Sector I/O. read/write handle `count` 512-byte sectors at absolute `lba`.
 * Return 0 on success, non-zero on error. write may be NULL for a read-only
 * mount (create/write/mkdir/unlink then fail with FAT32_EROFS). */
typedef struct fat32_io {
    void *ctx;
    int (*read)(void *ctx, uint32_t lba, void *buf, uint32_t count);
    int (*write)(void *ctx, uint32_t lba, const void *buf, uint32_t count);
} fat32_io_t;

typedef struct fat32_fs {
    fat32_io_t io;
    uint32_t partition_lba;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t fat_begin_lba;     /* first FAT sector                    */
    uint32_t cluster_begin_lba; /* data region (cluster 2) start        */
    uint32_t total_clusters;
    uint8_t scratch[FAT32_SECTOR_SIZE]; /* metadata scratch (FAT/dir)   */
} fat32_fs_t;

typedef struct fat32_file {
    fat32_fs_t *fs;
    uint32_t first_cluster;
    uint32_t size;        /* bytes                                      */
    uint32_t pos;         /* current byte offset                        */
    uint32_t cur_cluster; /* cluster containing pos (0 if none yet)     */
    uint32_t dir_lba;     /* sector holding this file's short dir entry */
    uint32_t dir_index;   /* entry index (0..15) within dir_lba         */
    int writable;
    int dirty; /* size/cluster changed -> flush on close               */
} fat32_file_t;

enum {
    FAT32_OK = 0,
    FAT32_EIO = -1,     /* sector read/write failed                     */
    FAT32_EINVAL = -2,  /* bad args / not FAT32 / bad path              */
    FAT32_ENOENT = -3,  /* path component not found                     */
    FAT32_EEXIST = -4,  /* already exists                               */
    FAT32_ENOSPC = -5,  /* no free cluster / dir slot                   */
    FAT32_EROFS = -6,   /* write op on a read-only mount                */
    FAT32_ENOTDIR = -7, /* a path component is not a directory          */
};

int fat32_mount(fat32_fs_t *fs, const fat32_io_t *io, uint32_t partition_lba);

/* Open an existing file for reading. */
int fat32_open(fat32_fs_t *fs, const char *path, fat32_file_t *f);

/* Create (or truncate to empty) a file for writing; parent dir must exist. */
int fat32_create(fat32_fs_t *fs, const char *path, fat32_file_t *f);

/* Open a file for writing positioned at end-of-file (creating it if absent);
 * subsequent writes append. Parent dir must exist. */
int fat32_append(fat32_fs_t *fs, const char *path, fat32_file_t *f);

/* Read up to len bytes at the current position; returns bytes read (0 = EOF)
 * or a negative FAT32_E* code. */
int fat32_read(fat32_file_t *f, void *buf, uint32_t len);

/* Write len bytes at the current position; returns bytes written or <0. */
int fat32_write(fat32_file_t *f, const void *buf, uint32_t len);

int fat32_seek(fat32_file_t *f, uint32_t pos);
uint32_t fat32_tell(const fat32_file_t *f);
uint32_t fat32_size(const fat32_file_t *f);

/* Flush metadata (size/first cluster) for a writable file. Called by close. */
int fat32_close(fat32_file_t *f);

/* Create a directory (parent must exist). */
int fat32_mkdir(fat32_fs_t *fs, const char *path);

/* Remove a file (not a directory). */
int fat32_unlink(fat32_fs_t *fs, const char *path);

/* Test whether a path exists; sets *is_dir if non-NULL. Returns FAT32_OK or
 * FAT32_ENOENT. */
int fat32_stat(fat32_fs_t *fs, const char *path, int *is_dir, uint32_t *size);

#endif /* _STM32F103_FAT32_H */
