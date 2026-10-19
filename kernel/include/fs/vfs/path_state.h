#ifndef _FS_VFS_PATH_STATE_H
#define _FS_VFS_PATH_STATE_H

#include "core/types.h"

/*
 * VFS path/name state engine.
 *
 * The algorithms are owned by kernel/fs/vfs/path_state.rs; path.c keeps the
 * exported names and the caller's buffers, and passes MAX_PATH_LEN in so the
 * MCU (32/64) and full (512) profiles share one engine.  There is no state
 * outside those buffers and no allocation.
 */

/* Containment test; `start == "/"` contains every absolute path. */
int a20_vfs_path_is_beneath(const char *start, const char *resolved);

/* Absolute `path` replaces the base; otherwise join onto `cwd` (or "/").
 * Truncates at `outsz - 1`, like the snprintf it replaces. */
int a20_vfs_path_join(const char *cwd, const char *path, char *out, size_t outsz);

/* Trim trailing slashes in place; "/" keeps its slash. */
void a20_vfs_path_trim_trailing_slashes(char *path);

/* Split into the parent directory and the final component. */
int a20_vfs_path_split_parent_name(const char *path, char *parent, size_t parent_sz,
                                   char *name, size_t name_sz, size_t max_path_len);

/* Collapse ".", ".." and empty components; `..` floors at "/". */
int a20_vfs_path_normalize_absolute(char *path, size_t max_path_len);

/* As above, but `..` may not climb above `root` (chroot / RESOLVE_IN_ROOT). */
int a20_vfs_path_normalize_absolute_with_root(char *path, const char *root,
                                              size_t max_path_len);

#endif /* _FS_VFS_PATH_STATE_H */
