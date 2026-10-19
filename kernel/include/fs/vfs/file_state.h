#ifndef _FS_VFS_FILE_STATE_H
#define _FS_VFS_FILE_STATE_H

#include "core/types.h"

/*
 * Open-file I/O-path policy.
 *
 * Whether a descriptor may read/write, whether a regular file's data goes
 * through the VFS page cache, and whether a write is buffered through the
 * page cache (ext4/uxfs, writable, hooks present) are owned by
 * kernel/fs/vfs/file_state.rs.  file.c keeps the vfile/vnode mount lookups
 * and the I/O.
 *
 * The mirrored filesystem-type and access-mode numbers are asserted against
 * the real definitions in file.c.
 */

int a20_vfs_should_read(int flags);
int a20_vfs_should_write(int flags);
int a20_vfs_file_uses_page_cache(int is_regular, int mnt_type);
int a20_vfs_file_uses_buffered_write(int is_regular, int mnt_type,
                                     uint32_t mnt_flags, int has_readpage,
                                     int has_writepage);

#endif /* _FS_VFS_FILE_STATE_H */
