#ifndef _FS_ANONFD_H
#define _FS_ANONFD_H

#include "fs/vfs.h"

int anonfd_install_vfile(vfile_t *vf, int flags);
int anonfd_free_priv_close(vfile_t *vf);

/* Create an anonymous in-memory file suitable for staging data (e.g. module
 * images).  Returns a global VFS fd or a negative errno.  The file is
 * read/write, supports lseek, and is only reachable through the returned fd
 * (no vnode is exposed to path lookup). */
int anonfd_create(int flags);

#endif /* _FS_ANONFD_H */
