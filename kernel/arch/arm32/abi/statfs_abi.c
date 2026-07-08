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

int arch_copy_statfs64_to_user(void *buf, int fs_type) {
    arm32_linux_statfs64_t sb;
    memset(&sb, 0, sizeof(sb));
    switch (fs_type) {
    case FS_TYPE_EXT4: sb.f_type = EXT4_SUPER_MAGIC; break;
    case FS_TYPE_FAT32: sb.f_type = 0x4d44; break;
    case FS_TYPE_PROCFS: sb.f_type = 0x9fa0; break;
    case FS_TYPE_DEVFS: sb.f_type = 0x01021994; break;
    case FS_TYPE_RAMFS:
    default: sb.f_type = 0x858458f6; break;
    }
    sb.f_bsize = PAGE_SIZE;
    sb.f_frsize = PAGE_SIZE;
    sb.f_blocks = 1024 * 1024;
    sb.f_bfree = 512 * 1024;
    sb.f_bavail = 512 * 1024;
    sb.f_files = VFS_MAX_OPEN;
    sb.f_ffree = VFS_MAX_OPEN / 2;
    sb.f_namelen = MAX_NAME_LEN;
    return copy_to_user(buf, &sb, sizeof(sb));
}
