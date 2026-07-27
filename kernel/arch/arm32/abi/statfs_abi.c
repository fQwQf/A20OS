#include "abi/linux/stat_abi.h"
#include "core/string.h"
#include "sys/usercopy.h"
#include "fs/vfs.h"
#include "core/consts.h"

typedef struct {
    int32_t f_type;
    int32_t f_bsize;
    uint32_t f_blocks;
    uint32_t f_bfree;
    uint32_t f_bavail;
    uint32_t f_files;
    uint32_t f_ffree;
    int32_t f_fsid[2];
    int32_t f_namelen;
    int32_t f_frsize;
    int32_t f_flags;
    int32_t f_spare[4];
} arm32_linux_statfs64_t;

int arch_copy_statfs64_to_user(void *buf, const kstatfs_t *kst) {
    arm32_linux_statfs64_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.f_type = (int32_t)kst->f_type;
    sb.f_bsize = (int32_t)kst->f_bsize;
    sb.f_blocks = (uint32_t)kst->f_blocks;
    sb.f_bfree = (uint32_t)kst->f_bfree;
    sb.f_bavail = (uint32_t)kst->f_bavail;
    sb.f_files = (uint32_t)kst->f_files;
    sb.f_ffree = (uint32_t)kst->f_ffree;
    sb.f_fsid[0] = (int32_t)kst->f_fsid;
    sb.f_fsid[1] = (int32_t)(kst->f_fsid >> 32);
    sb.f_namelen = (int32_t)kst->f_namelen;
    sb.f_frsize = (int32_t)kst->f_frsize;
    sb.f_flags = (int32_t)kst->f_flags;
    return copy_to_user(buf, &sb, sizeof(sb));
}
