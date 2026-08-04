#include "arch/common/stat_abi_64le.h"

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    uint64_t unused[3];
} ppc64_linux_stat_t;

_Static_assert(sizeof(ppc64_linux_stat_t) == 144,
               "PPC64 Linux struct stat must be 144 bytes");

void arch_copy_kstat_to_user(void *st, const kstat_t *kst)
{
    ppc64_linux_stat_t out;
    memset(&out, 0, sizeof(out));
    out.st_dev = kst->st_dev;
    out.st_ino = kst->st_ino;
    out.st_nlink = kst->st_nlink;
    out.st_mode = kst->st_mode;
    out.st_uid = kst->st_uid;
    out.st_gid = kst->st_gid;
    out.st_rdev = kst->st_rdev;
    out.st_size = kst->st_size;
    out.st_blksize = kst->st_blksize;
    out.st_blocks = kst->st_blocks;
    out.st_atime = kst->st_atime;
    out.st_atime_nsec = kst->st_atime_nsec;
    out.st_mtime = kst->st_mtime;
    out.st_mtime_nsec = kst->st_mtime_nsec;
    out.st_ctime = kst->st_ctime;
    out.st_ctime_nsec = kst->st_ctime_nsec;
    copy_to_user(st, &out, sizeof(out));
}

int arch_copy_statfs64_to_user(void *buf, const kstatfs_t *kst)
{
    return arch_copy_statfs64_64le_to_user(buf, kst);
}
