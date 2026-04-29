#ifndef _FS_VFS_FILE_H
#define _FS_VFS_FILE_H

#include "fs/vfs.h"

int vfs_is_pipe_vfile(vfile_t *vf);
int vfs_is_char_device_vfile(vfile_t *vf);
int vfs_should_read(int flags);
int vfs_should_write(int flags);

#endif /* _FS_VFS_FILE_H */
