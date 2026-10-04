#ifndef _FS_FILE_H
#define _FS_FILE_H

#include "core/types.h"

/* Declared with struct vfile to avoid an include cycle: fs/vfs.h (which
 * typedefs vfile_t) includes this header. */
struct vfile;

void file_table_init(void);
size_t vfile_live_count(void);
struct vfile *vfile_alloc(void);
void vfile_free(struct vfile *vf);
void vfile_ref_init(struct vfile *vf, int refs);
void vfile_get(struct vfile *vf);
int vfile_ref_read(struct vfile *vf);
int vfile_put_ref_only(struct vfile *vf);

/* fd resolution: @fd is the calling task's fd, resolved through its
 * files_struct (fs/fdtable.c).  vfs_get_file_ref returns a referenced vfile
 * or NULL; vfs_put_file_ref/vfs_put_file drop the reference and run the
 * close finalizer when it was the last. */
struct vfile *vfs_get_file_ref(int fd);
void vfs_put_file_ref(int fd, struct vfile *vf);
void vfs_put_file(struct vfile *vf);
/* Run by the final ref drop: fire EventQ destroy hooks, run the file's
 * close op, free the vfile and drop the vnode.  Defined in fs/vfs.c;
 * called from fs/file.c and fs/fdtable.c. */
void vfs_finalize_closed_vfile(struct vfile *vf);

#endif /* _FS_FILE_H */
