#ifndef _ABI_LINUX_STAT_H
#define _ABI_LINUX_STAT_H

#define STATX_TYPE         0x0001U
#define STATX_MODE         0x0002U
#define STATX_NLINK        0x0004U
#define STATX_UID          0x0008U
#define STATX_GID          0x0010U
#define STATX_ATIME        0x0020U
#define STATX_MTIME        0x0040U
#define STATX_CTIME        0x0080U
#define STATX_INO          0x0100U
#define STATX_SIZE         0x0200U
#define STATX_BLOCKS       0x0400U
#define STATX_BASIC_STATS  0x07ffU
#define STATX_BTIME        0x0800U
#define STATX_ALL          0x0fffU
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10

#endif /* _ABI_LINUX_STAT_H */
