#ifndef _FS_VFS_PATH_H
#define _FS_VFS_PATH_H

#include "core/types.h"

int vfs_path_join(const char *cwd, const char *path, char *out, size_t outsz);
void vfs_path_trim_trailing_slashes(char *path);
int vfs_path_split_parent_name(const char *path, char *parent, size_t parent_sz,
                               char *name, size_t name_sz);
int vfs_path_normalize_absolute(char *path);

#endif
