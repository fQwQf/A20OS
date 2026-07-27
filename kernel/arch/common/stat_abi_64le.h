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

static inline void fill_linux_statfs64_64le(linux_statfs64_64le_t *sb,
                                            const kstatfs_t *kst)
{
    memset(sb, 0, sizeof(*sb));
    sb->f_type = kst->f_type;
    sb->f_bsize = kst->f_bsize;
    sb->f_blocks = kst->f_blocks;
    sb->f_bfree = kst->f_bfree;
    sb->f_bavail = kst->f_bavail;
    sb->f_files = kst->f_files;
    sb->f_ffree = kst->f_ffree;
    sb->f_fsid[0] = (int32_t)kst->f_fsid;
    sb->f_fsid[1] = (int32_t)(kst->f_fsid >> 32);
    sb->f_namelen = kst->f_namelen;
    sb->f_frsize = kst->f_frsize;
    sb->f_flags = kst->f_flags;
}

static inline int arch_copy_statfs64_64le_to_user(void *buf,
                                                  const kstatfs_t *kst)
{
    linux_statfs64_64le_t sb;
    fill_linux_statfs64_64le(&sb, kst);
    return copy_to_user(buf, &sb, sizeof(sb));
}

#endif /* _ARCH_COMMON_STAT_ABI_64LE_H */
