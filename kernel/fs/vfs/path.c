#include "fs/vfs/path.h"
#include "fs/vfs/path_state.h"

#include "core/consts.h"
#include "core/stdio.h"
#include "core/string.h"

int path_is_beneath(const char *start, const char *resolved) {
    /* Owned by path_state.rs. */
    return a20_vfs_path_is_beneath(start, resolved);
}

int vfs_path_join(const char *cwd, const char *path, char *out, size_t outsz) {
    return a20_vfs_path_join(cwd, path, out, outsz);
}

void vfs_path_trim_trailing_slashes(char *path) {
    a20_vfs_path_trim_trailing_slashes(path);
}

int vfs_path_split_parent_name(const char *path, char *parent, size_t parent_sz,
                               char *name, size_t name_sz) {
    return a20_vfs_path_split_parent_name(path, parent, parent_sz, name, name_sz,
                                          MAX_PATH_LEN);
}

int vfs_path_normalize_absolute(char *path) {
    return a20_vfs_path_normalize_absolute(path, MAX_PATH_LEN);
}

int vfs_path_normalize_absolute_with_root(char *path, const char *root) {
    return a20_vfs_path_normalize_absolute_with_root(path, root, MAX_PATH_LEN);
}
