#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "sys/usercopy.h"

typedef struct {
    uint64_t st_dev;
    uint32_t __pad1;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t __pad2;
    int32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} arm32_linux_stat_t;

void arch_copy_kstat_to_user(void *st, const kstat_t *kst) {
    arm32_linux_stat_t out;
    memset(&out, 0, sizeof(out));
    out.st_dev = kst->st_dev;
    out.st_ino = (uint32_t)kst->st_ino;
    out.st_mode = kst->st_mode;
    out.st_nlink = kst->st_nlink;
    out.st_uid = kst->st_uid;
    out.st_gid = kst->st_gid;
    out.st_rdev = kst->st_rdev;
    out.st_size = (int32_t)kst->st_size;
    out.st_blksize = (uint32_t)kst->st_blksize;
    out.st_blocks = (uint32_t)kst->st_blocks;
    out.st_atime = (uint32_t)kst->st_atime;
    out.st_atime_nsec = (uint32_t)kst->st_atime_nsec;
    out.st_mtime = (uint32_t)kst->st_mtime;
    out.st_mtime_nsec = (uint32_t)kst->st_mtime_nsec;
    out.st_ctime = (uint32_t)kst->st_ctime;
    out.st_ctime_nsec = (uint32_t)kst->st_ctime_nsec;
    copy_to_user(st, &out, sizeof(out));
}
