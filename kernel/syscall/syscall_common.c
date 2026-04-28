#include "syscall_internal.h"

int syscall_sig_diag_count = 0;
int syscall_sleep_diag_count = 0;

int64_t syscall_get_global_fd(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    return gfd;
}

int syscall_alloc_local_fd(task_t *t, int gfd) {
    if (!t || gfd < 0) return -EBADF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] < 0) {
            t->fd_table[i] = gfd;
            t->fd_cloexec[i] = 0;
            return i;
        }
    }
    vfs_close(gfd);
    return -EMFILE;
}

int syscall_alloc_local_fd_with_flags(task_t *t, int gfd, int flags) {
    int fd = syscall_alloc_local_fd(t, gfd);
    if (fd >= 0)
        t->fd_cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
    return fd;
}

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
        base = t->cwd[0] ? t->cwd : "/";
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
        int gfd = t->fd_table[dirfd];
        if (gfd < 0) return -EBADF;
        vfile_t *vf = vfs_get_file(gfd);
        if (!vf || !vf->vnode) return -EBADF;
        if (vf->vnode->type != VFS_FT_DIR) return -ENOTDIR;
        if (!vf->path[0]) return -EINVAL;
        base = vf->path;
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

    const char *root = t->root_path[0] ? t->root_path : "/";
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
