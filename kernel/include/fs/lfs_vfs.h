#ifndef _FS_LFS_VFS_H
#define _FS_LFS_VFS_H

/*
 * littlefs VFS adapter (kernel/fs/diskfs/lfs_vfs.c).  Power-loss resilient
 * embedded filesystem on any block_dev_t-backed bcache; see the adapter
 * header comment for the block-geometry and ino decisions.
 */

struct bcache;
struct vnode;
struct vfile;

struct vnode *littlefs_mount(struct bcache *bc);
void          littlefs_unmount(struct vnode *root);
/* lfs_file_sync + device flush for the fsync path */
int           lfs_vfs_fsync_vfile(struct vfile *vf);

#endif /* _FS_LFS_VFS_H */
