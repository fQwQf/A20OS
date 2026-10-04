#include "syscall_internal.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/path.h"

int syscall_sig_diag_count = 0;
int syscall_sleep_diag_count = 0;

/*
 * Build `base` + `rel` into `dst`, inserting one '/' between them when `sep`
 * is set and `base` does not already end in one. Returns the length written,
 * or -1 when the result would not fit in `dstsz`.
 *
 * Path resolution runs this on every path-taking syscall, so it moves exactly
 * the bytes involved instead of walking a "%s" format; the no-op cases (empty
 * rel, no separator wanted) degrade to a single strcpy of `base`.
 */
static int path_build(char *dst, size_t dstsz, const char *base, const char *rel, int sep)
{
    size_t blen = strlen(base);
    size_t rlen = strlen(rel);
    if (sep && rlen > 0 && blen > 0 && base[blen - 1] != '/')
        sep = 1;
    else
        sep = 0;
    size_t total = blen + (size_t)sep + rlen;
    if (total >= dstsz)
        return -1;
    memcpy(dst, base, blen);
    if (sep)
        dst[blen] = '/';
    memcpy(dst + blen + sep, rel, rlen);
    dst[total] = '\0';
    return (int)total;
}

int syscall_path_at(int dirfd, const char *path, char *out, size_t outsz) {
    if (!path || !out || outsz == 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    if (strlen(path) >= outsz - 1)
        return -ENAMETOOLONG;

    /* An absolute path is already the task-logical path, so it is joined
     * straight into `out` below rather than staged in a second buffer. */
    const char *logical = path;
    bool dirfd_path_is_physical = false;
    char base_buf[MAX_PATH_LEN];
    char joined[MAX_PATH_LEN];
    if (path[0] != '/') {
        const char *base = NULL;
        if (dirfd == AT_FDCWD) {
            base = t->fs.cwd[0] ? t->fs.cwd : "/";
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
            } else {
                strncpy(base_buf, vf->path, sizeof(base_buf) - 1);
                base_buf[sizeof(base_buf) - 1] = '\0';
            }
            vfs_put_file_ref(gfd, vf);
            /*
             * vfile::path is stored in the global VFS namespace.  In particular,
             * after chroot it already contains root_path.  Keep that provenance:
             * treating it as a task-logical path would prefix root_path twice.
             */
            base = base_buf;
            dirfd_path_is_physical = true;
        }
        if (path_build(joined, sizeof(joined), base, path, 1) < 0)
            return -ENAMETOOLONG;
        logical = joined;
    }

    const char *root = t->fs.root_path[0] ? t->fs.root_path : "/";
    int n;
    if (dirfd_path_is_physical) {
        if (strcmp(root, "/") != 0 && !path_is_beneath(root, base_buf))
            return -EACCES;
        n = path_build(out, outsz, logical, "", 0);
    } else if (strcmp(root, "/") == 0) {
        n = path_build(out, outsz, logical, "", 0);
    } else if (strcmp(logical, "/") == 0) {
        n = path_build(out, outsz, root, "", 0);
    } else {
        n = path_build(out, outsz, root, logical, 0);
    }
    if (n < 0)
        return -ENAMETOOLONG;
    out[outsz - 1] = '\0';
    if (strcmp(root, "/") != 0)
        vfs_path_normalize_absolute_with_root(out, root);
    return 0;
}