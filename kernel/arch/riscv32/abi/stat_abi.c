#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "sys/usercopy.h"

void arch_copy_kstat_to_user(void *st, const kstat_t *kst) {
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    *(uint64_t *)(buf + 0) = kst->st_dev;
    *(uint32_t *)(buf + 8) = 0;
    *(uint64_t *)(buf + 12) = kst->st_ino;
    *(uint32_t *)(buf + 20) = kst->st_mode;
    *(uint32_t *)(buf + 24) = kst->st_nlink;
    *(uint32_t *)(buf + 28) = kst->st_uid;
    *(uint32_t *)(buf + 32) = kst->st_gid;
    *(uint64_t *)(buf + 36) = kst->st_rdev;
    *(uint32_t *)(buf + 44) = 0;
    *(uint64_t *)(buf + 48) = kst->st_size;
    *(uint32_t *)(buf + 56) = kst->st_blksize;
    *(uint32_t *)(buf + 60) = 0;
    *(uint64_t *)(buf + 64) = kst->st_blocks;
    *(uint32_t *)(buf + 72) = (uint32_t)kst->st_atime;
    *(uint32_t *)(buf + 76) = (uint32_t)kst->st_atime_nsec;
    *(uint32_t *)(buf + 80) = (uint32_t)kst->st_mtime;
    *(uint32_t *)(buf + 84) = (uint32_t)kst->st_mtime_nsec;
    *(uint32_t *)(buf + 88) = (uint32_t)kst->st_ctime;
    *(uint32_t *)(buf + 92) = (uint32_t)kst->st_ctime_nsec;
    copy_to_user(st, buf, sizeof(buf));
}
