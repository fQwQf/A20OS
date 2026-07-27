#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "core/consts.h"
#include "sys/usercopy.h"

typedef struct {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
} linux_statfs64_rv32_t;

int arch_copy_statfs64_to_user(void *buf, const kstatfs_t *kst) {
    linux_statfs64_rv32_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.f_type = (uint32_t)kst->f_type;
    sb.f_bsize = (uint32_t)kst->f_bsize;
    sb.f_blocks = kst->f_blocks;
    sb.f_bfree = kst->f_bfree;
    sb.f_bavail = kst->f_bavail;
    sb.f_files = kst->f_files;
    sb.f_ffree = kst->f_ffree;
    sb.f_fsid[0] = (int32_t)kst->f_fsid;
    sb.f_fsid[1] = (int32_t)(kst->f_fsid >> 32);
    sb.f_namelen = (uint32_t)kst->f_namelen;
    sb.f_frsize = (uint32_t)kst->f_frsize;
    sb.f_flags = (uint32_t)kst->f_flags;
    return copy_to_user(buf, &sb, sizeof(sb));
}
