#ifndef _FS_FILE_H
#define _FS_FILE_H

#include "fs/vfs.h"

void file_table_init(void);
size_t file_open_fd_count(void);
size_t vfile_live_count(void);
vfile_t *vfile_alloc(void);
void vfile_free(vfile_t *vf);
void vfile_ref_init(vfile_t *vf, int refs);
void vfile_get(vfile_t *vf);
int vfile_ref_read(vfile_t *vf);
int vfile_put_ref_only(vfile_t *vf);
vfile_t *vfs_get_file_ref(int fd);
void vfs_put_file_ref(int fd, vfile_t *vf);
void vfs_put_file(vfile_t *vf);
int vfs_ref_fd(int fd);
int file_install_at(int fd, vfile_t *vf);
int file_close_prepare(int fd, vfile_t **closed);
int file_put_ref_prepare(int fd, vfile_t *vf, vfile_t **closed);
int vfs_dupfd(int fd, int minfd);

#endif /* _FS_FILE_H */
