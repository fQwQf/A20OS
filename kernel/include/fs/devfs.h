#ifndef _DEVFS_H
#define _DEVFS_H

#include "fs/vfs.h"

vnode_t *devfs_mount(void);
vfile_t *devfs_open_vnode(vnode_t *vn, int flags);
vfile_t *devfs_create_stdio(int fd);
int devfs_is_char_vfile(vfile_t *vf);
int devfs_is_tty_vfile(vfile_t *vf);
int devfs_is_zero_vfile(vfile_t *vf);

#endif /* _DEVFS_H */
