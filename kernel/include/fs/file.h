#ifndef _FS_FILE_H
#define _FS_FILE_H

#include "fs/vfs.h"

void file_table_init(void);
vfile_t *vfile_alloc(void);
void vfile_free(vfile_t *vf);
vfile_t *vfs_get_file_ref(int fd);
void vfs_put_file_ref(int fd, vfile_t *vf);
int vfs_ref_fd(int fd);
int file_install_at(int fd, vfile_t *vf);
int file_close_prepare(int fd, vfile_t **closed);
int vfs_dupfd(int fd, int minfd);

#endif /* _FS_FILE_H */
