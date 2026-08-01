#ifndef _FS_ISOFS_H
#define _FS_ISOFS_H

#include "core/types.h"
#include "core/sync.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"

#define ISOFS_BLOCK_SIZE      2048U
#define ISOFS_VD_OFFSET       16U
#define ISO9660_ID            "CD001"
#define HIGH_SIERRA_ID        "CDROM"

#define ISO_VD_BOOT_RECORD    0
#define ISO_VD_PRIMARY        1
#define ISO_VD_SUPPLEMENTARY  2
#define ISO_VD_TERMINATOR     255

#define ISO_FLAG_DIRECTORY    0x02U
#define ISO_FLAG_MULTI_EXTENT 0x80U

/* Both-endian numeric helpers: ISO stores LE then BE; we read the LE half. */
static inline uint16_t iso_721(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t iso_733(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Primary volume descriptor (ISO 9660). */
typedef struct __attribute__((packed)) iso_primary_descriptor {
    uint8_t  type;
    uint8_t  id[5];
    uint8_t  version;
    uint8_t  unused1;
    uint8_t  system_id[32];
    uint8_t  volume_id[32];
    uint8_t  unused2[8];
    uint8_t  volume_space_size[8];    /* 733 */
    uint8_t  unused3[32];
    uint8_t  volume_set_size[4];      /* 723 */
    uint8_t  volume_seq_number[4];    /* 723 */
    uint8_t  logical_block_size[4];   /* 723 */
    uint8_t  path_table_size[8];      /* 733 */
    uint8_t  type_l_path_table[4];    /* 731 */
    uint8_t  opt_type_l_path_table[4];
    uint8_t  type_m_path_table[4];    /* 732 */
    uint8_t  opt_type_m_path_table[4];
    uint8_t  root_directory_record[34];
    uint8_t  reserved[1856];
} iso_primary_descriptor_t;

/* Directory record. */
typedef struct __attribute__((packed)) iso_directory_record {
    uint8_t  length;                  /* 0 = end of directory */
    uint8_t  ext_attr_length;         /* in logical blocks */
    uint8_t  extent[8];               /* 733: LBA of data/dir contents */
    uint8_t  size[8];                 /* 733: file size in bytes */
    uint8_t  date[7];
    uint8_t  flags;
    uint8_t  file_unit_size;
    uint8_t  interleave;
    uint8_t  vol_seq_number[4];       /* 723 */
    uint8_t  name_len;
    uint8_t  name[];
} iso_directory_record_t;

#define ISO_DIR_REC_HEADER 33U

typedef struct isofs_sb {
    bcache_t *bc;
    uint32_t  block_size;             /* logical block size from PVD */
    uint32_t  vol_space;              /* volume space size in blocks */
    uint32_t  root_extent;            /* root dir extent (LBA) */
    uint32_t  root_size;              /* root dir size in bytes */
    mutex_t   lock;
} isofs_sb_t;

typedef struct isofs_vnode_priv {
    isofs_sb_t *sb;
    uint32_t    extent;               /* first extent LBA */
    uint64_t    file_size;
    int         is_dir;
} isofs_vnode_priv_t;

typedef struct isofs_fctx {
    isofs_sb_t *sb;
    uint32_t    extent;
    uint64_t    file_size;
    int         is_dir;
    size_t      file_off;
    size_t      dir_off;
} isofs_fctx_t;

vnode_t *isofs_mount(bcache_t *bc);
void     isofs_unmount(vnode_t *root);

#endif /* _FS_ISOFS_H */
