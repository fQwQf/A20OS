#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "sys/usercopy.h"

/*
 * x86_64 Linux struct stat layout (128 bytes, glibc/musl):
 *   0:  st_dev     u64
 *   8:  st_ino     u64
 *  16:  st_nlink   u64
 *  24:  st_mode    u32
 *  28:  st_uid     u32
 *  32:  st_gid     u32
 *  36:  __pad0     u32
 *  40:  st_rdev    u64
 *  48:  st_size    i64
 *  56:  st_blksize i64
 *  64:  st_blocks  i64
 *  72:  st_atime   u64
 *  80:  st_atime_nsec u64
 *  88:  st_mtime   u64
 *  96:  st_mtime_nsec u64
 * 104:  st_ctime   u64
 * 112:  st_ctime_nsec u64
 * 120:  __unused   i64[3]
 */
void arch_copy_kstat_to_user(void *st, const kstat_t *kst)
{
    uint64_t buf64[128 / 8];
    memset(buf64, 0, sizeof(buf64));
    uint8_t *buf = (uint8_t *)buf64;
    uint64_t *u64 = (uint64_t *)buf;
    uint32_t *u32 = (uint32_t *)buf;

    u64[0]  = kst->st_dev;
    u64[1]  = kst->st_ino;
    u64[2]  = kst->st_nlink;
    u32[6]  = kst->st_mode;
    u32[7]  = kst->st_uid;
    u32[8]  = kst->st_gid;
    u64[5]  = kst->st_rdev;
    u64[6]  = kst->st_size;
    u64[7]  = kst->st_blksize;
    u64[8]  = kst->st_blocks;
    u64[9]  = kst->st_atime;
    u64[10] = kst->st_atime_nsec;
    u64[11] = kst->st_mtime;
    u64[12] = kst->st_mtime_nsec;
    u64[13] = kst->st_ctime;
    u64[14] = kst->st_ctime_nsec;
    copy_to_user(st, buf, sizeof(buf64));
}
