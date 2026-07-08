#ifndef _ARCH_COMMON_STAT_ABI_64LE_H
#define _ARCH_COMMON_STAT_ABI_64LE_H

#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "sys/usercopy.h"

static inline void arch_copy_kstat64le_to_user(void *st, const kstat_t *kst)
{
    uint64_t buf64[128 / 8];
    memset(buf64, 0, sizeof(buf64));
    uint8_t *buf = (uint8_t *)buf64;
    uint64_t *u64 = (uint64_t *)buf;
    uint32_t *u32 = (uint32_t *)buf;

    u64[0]  = kst->st_dev;
    u64[1]  = kst->st_ino;
    u32[4]  = kst->st_mode;
    u32[5]  = kst->st_nlink;
    u32[6]  = kst->st_uid;
    u32[7]  = kst->st_gid;
    u64[4]  = kst->st_rdev;
    u64[5]  = 0;
    u64[6]  = kst->st_size;
    u32[14] = kst->st_blksize;
    u32[15] = 0;
    u64[8]  = kst->st_blocks;
    u64[9]  = kst->st_atime;
    u64[10] = kst->st_atime_nsec;
    u64[11] = kst->st_mtime;
    u64[12] = kst->st_mtime_nsec;
    u64[13] = kst->st_ctime;
    u64[14] = kst->st_ctime_nsec;
    u32[30] = 0;
    u32[31] = 0;
    copy_to_user(st, buf, sizeof(buf64));
}

typedef struct linux_statfs64_64le {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
} linux_statfs64_64le_t;

static inline void fill_linux_statfs64_64le(linux_statfs64_64le_t *sb, int fs_type)
{
    memset(sb, 0, sizeof(*sb));
    switch (fs_type) {
    case FS_TYPE_EXT4: sb->f_type = EXT4_SUPER_MAGIC; break;
    case FS_TYPE_FAT32: sb->f_type = 0x4d44; break;
    case FS_TYPE_PROCFS: sb->f_type = 0x9fa0; break;
    case FS_TYPE_DEVFS: sb->f_type = 0x01021994; break;
    case FS_TYPE_RAMFS:
    default: sb->f_type = 0x858458f6; break;
    }
    sb->f_bsize = PAGE_SIZE;
    sb->f_frsize = PAGE_SIZE;
    sb->f_blocks = 1024 * 1024;
    sb->f_bfree = 512 * 1024;
    sb->f_bavail = 512 * 1024;
    sb->f_files = VFS_MAX_OPEN;
    sb->f_ffree = VFS_MAX_OPEN / 2;
    sb->f_namelen = MAX_NAME_LEN;
}

static inline int arch_copy_statfs64_64le_to_user(void *buf, int fs_type)
{
    linux_statfs64_64le_t sb;
    fill_linux_statfs64_64le(&sb, fs_type);
    return copy_to_user(buf, &sb, sizeof(sb));
}

#endif /* _ARCH_COMMON_STAT_ABI_64LE_H */
