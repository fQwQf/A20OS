#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "proc/proc.h"
#include "core/errno.h"
#include "core/string.h"

/*
 * file_getattr(2) / file_setattr(2) core (LoongArch-only Linux syscalls).
 *
 * These are the syscall forms of the FS_IOC_GETFLAGS/SETFLAGS ioctl family,
 * operating on a generic `struct fileattr`.  The wire layout mirrors the
 * Linux uapi (include/uapi/linux/fs.h):
 *
 *   struct fileattr {
 *       u32 valid;        bitfield of valid fields
 *       u32 flags;        generic inode flags (FS_*_FL)
 *       u32 fsx_xflags;   fsx flags
 *       u32 fspare;
 *       u32 gfs2_acl;
 *       u32 version;
 *       u32 flags_mask;   flags the filesystem allows
 *       u32 flags_ro;     flags that are read-only
 *       u32 xflags_mask;  allowed xflags
 *       u32 xflags_ro;    read-only xflags
 *   };
 *
 *   FILEATTR_VALID_FLAGS   = 1
 *   FILEATTR_VALID_XFLAGS  = 2
 *
 * A20OS vnodes currently carry no FS_IOC_GETFLAGS-style attribute word, so
 * the core reports an empty attribute set (valid = FILEATTR_VALID_FLAGS with
 * flags = 0, all masks zero) and refuses to set any attribute.  This matches
 * Linux filesystems with no attribute support and keeps the ABI honest: a
 * probe sees flags=0, and an attempt to set a flag fails with -EOPNOTSUPP.
 */

/* FILEATTR flags from Linux uapi/linux/fs.h. */
#define FS_SECRM_FL        0x00000001
#define FS_UNRM_FL         0x00000002
#define FS_COMPR_FL        0x00000004
#define FS_SYNC_FL         0x00000008
#define FS_IMMUTABLE_FL    0x00000010
#define FS_APPEND_FL       0x00000020
#define FS_NODUMP_FL       0x00000040
#define FS_NOATIME_FL      0x00000080
#define FS_DIRTY_FL        0x00000100
#define FS_COMPRBLK_FL     0x00000200
#define FS_NOCOMP_FL       0x00000400
#define FS_ENCRYPT_FL      0x00000800
#define FS_BTREE_FL        0x00001000
#define FS_INDEX_FL        0x00001000
#define FS_IMAGIC_FL       0x00002000
#define FS_JOURNAL_DATA_FL 0x00004000
#define FS_NOTAIL_FL       0x00008000
#define FS_DIRSYNC_FL      0x00010000
#define FS_TOPDIR_FL       0x00020000
#define FS_HUGE_FILE_FL    0x00040000
#define FS_EXTENT_FL       0x00080000
#define FS_VERITY_FL       0x00100000
#define FS_EA_INODE_FL     0x00200000
#define FS_EOFBLOCKS_FL    0x00400000
#define FS_NOCOW_FL        0x00800000
#define FS_DAX_FL          0x02000000
#define FS_INLINE_DATA_FL  0x10000000
#define FS_PROJINHERIT_FL  0x20000000
#define FS_CASEFOLD_FL     0x40000000

#define FILEATTR_VALID_FLAGS  0x00000001
#define FILEATTR_VALID_XFLAGS 0x00000002

/* Shared ABI type so the wire layout lives in one place. */
void vfs_fileattr_init(a20_fileattr_t *fa)
{
    memset(fa, 0, sizeof(*fa));
}

/* Report the attribute word for a vnode.  A20OS tracks none, so flags is 0
 * but the flags field is reported valid so userspace sees a well-formed
 * (empty) attribute set. */
void vfs_fileattr_get(a20_fileattr_t *fa)
{
    vfs_fileattr_init(fa);
    fa->valid = FILEATTR_VALID_FLAGS;
    fa->flags = 0;
    fa->flags_mask = 0;   /* no attribute bits supported */
    fa->xflags_mask = 0;
}

/* Apply a requested attribute word.  Since no flags are supported, any
 * attempt to set a flag is refused with -EOPNOTSUPP. */
int vfs_fileattr_set(const a20_fileattr_t *fa)
{
    if (!fa)
        return -EINVAL;
    if (fa->valid & ~(FILEATTR_VALID_FLAGS | FILEATTR_VALID_XFLAGS))
        return -EINVAL;
    if (fa->valid & FILEATTR_VALID_FLAGS) {
        if (fa->flags != 0)
            return -EOPNOTSUPP;
    }
    if (fa->valid & FILEATTR_VALID_XFLAGS) {
        if (fa->fsx_xflags != 0)
            return -EOPNOTSUPP;
    }
    return 0;
}
