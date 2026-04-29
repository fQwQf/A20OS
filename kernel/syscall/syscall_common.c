#include "syscall_internal.h"

int syscall_sig_diag_count = 0;
int syscall_sleep_diag_count = 0;

int syscall_path_at(int dirfd, const char *path, char *out, size_t outsz) {
    if (!path || !out || outsz == 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    if (strlen(path) >= outsz - 1)
        return -ENAMETOOLONG;

    const char *base = NULL;
    char logical[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(logical, path, sizeof(logical) - 1);
        logical[sizeof(logical) - 1] = '\0';
    } else if (dirfd == AT_FDCWD) {
        base = t->fs.cwd[0] ? t->fs.cwd : "/";
        size_t len = strlen(base);
        int n;
        if (path[0] == '\0') {
            n = snprintf(logical, sizeof(logical), "%s", base);
        } else if (len > 0 && base[len - 1] == '/') {
            n = snprintf(logical, sizeof(logical), "%s%s", base, path);
        } else {
            n = snprintf(logical, sizeof(logical), "%s/%s", base, path);
        }
        if (n < 0 || (size_t)n >= sizeof(logical))
            return -ENAMETOOLONG;
    } else {
        if (dirfd < 0 || dirfd >= MAX_FILES) return -EBADF;
        int gfd = fdtable_get(t, dirfd);
        if (gfd < 0) return -EBADF;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (!vf) return -EBADF;
        if (!vf->vnode) {
            vfs_put_file_ref(gfd, vf);
            return -EBADF;
        }
        if (vf->vnode->type != VFS_FT_DIR) {
            vfs_put_file_ref(gfd, vf);
            return -ENOTDIR;
        }
        if (!vf->path[0]) {
            vfs_put_file_ref(gfd, vf);
            return -EINVAL;
        }
        strncpy(logical, vf->path, sizeof(logical) - 1);
        logical[sizeof(logical) - 1] = '\0';
        vfs_put_file_ref(gfd, vf);
        base = logical;
        size_t len = strlen(base);
        int n;
        if (path[0] == '\0') {
            n = snprintf(logical, sizeof(logical), "%s", base);
        } else if (len > 0 && base[len - 1] == '/') {
            n = snprintf(logical, sizeof(logical), "%s%s", base, path);
        } else {
            n = snprintf(logical, sizeof(logical), "%s/%s", base, path);
        }
        if (n < 0 || (size_t)n >= sizeof(logical))
            return -ENAMETOOLONG;
    }

    const char *root = t->fs.root_path[0] ? t->fs.root_path : "/";
    int n;
    if (strcmp(root, "/") == 0) {
        n = snprintf(out, outsz, "%s", logical);
    } else if (strcmp(logical, "/") == 0) {
        n = snprintf(out, outsz, "%s", root);
    } else {
        n = snprintf(out, outsz, "%s%s", root, logical);
    }
    if (n < 0 || (size_t)n >= outsz)
        return -ENAMETOOLONG;
    out[outsz - 1] = '\0';
    return 0;
}
