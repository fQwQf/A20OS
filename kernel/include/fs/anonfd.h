#ifndef _FS_ANONFD_H
#define _FS_ANONFD_H

#include "fs/vfs.h"

int anonfd_install_vfile(vfile_t *vf, int flags);
int anonfd_free_priv_close(vfile_t *vf);

#endif /* _FS_ANONFD_H */
