#ifndef _FS_VFS_STAT_PERM_H
#define _FS_VFS_STAT_PERM_H

#include "fs/vfs.h"
#include "proc/proc.h"

void fill_char_kstat(kstat_t *st);
void fill_pipe_kstat(kstat_t *st);

int vfs_task_in_group(task_t *t, uint32_t gid);
int vfs_mode_has_perm_ids_nocap(uint32_t st_mode, uint32_t file_uid,
                                uint32_t file_gid, uint32_t uid,
                                uint32_t gid, int mask);
int vfs_mode_has_perm_ids(uint32_t st_mode, uint32_t file_uid,
                          uint32_t file_gid, uint32_t uid,
                          uint32_t gid, int mask);
int vfs_vnode_stat(vnode_t *vn, kstat_t *st);
int vfs_vnode_permission(vnode_t *vn, int mask);
int vfs_current_owns(vnode_t *vn);
int vfs_sticky_may_remove(vnode_t *dir, vnode_t *victim);
void vfs_touch_mtime(vnode_t *vn);
int vfs_set_times(vnode_t *vn, const uint64_t times[4]);

#endif /* _FS_VFS_STAT_PERM_H */
