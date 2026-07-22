/*
 * A20OS — Virtual Filesystem (VFS)
 *
 * Provides a unified fd-based interface over:
 *   - ramfs  (always present, mounted at /)
 *   - fat32  (mounted at /mnt or wherever the block device is)
 *   - devfs  (virtual devices: stdin, stdout, stderr, null, zero)
 *
 * All process file descriptors go through this layer.
 * Inspired by RocketOS fs/ and Linux VFS.
 */

#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/file.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/path.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/locks.h"
#include "fs/page_cache.h"
#include "fs/pipe.h"
#include "fs/ramfs.h"
#include "fs/devfs.h"
#include "fs/fat32.h"
#include "fs/ext4.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "drivers/block/virtio_blk.h"
#include "fs/block_cache.h"
#include "drivers/block/virtio_blk.h"
#include "fs/block_cache.h"
#include "net/socket.h"


vnode_t *vfs_resolve_no_follow_final(const char *path);

/*
 * VFS_CONCURRENCY_SMOKE_MATRIX:
 * - Static gate covers close/read/write reference helpers, fd dup/close paths,
 *   rename/unlink/open dcache invalidation, symlink loop handling, and
 *   mount/unmount root-reference release rules.
 * - Runtime expansion should add parallel close/read/write, rename/unlink/open,
 *   symlink loop, mount/unmount, and dup/close_range smoke once the harness can
 *   drive filesystem workloads under QEMU deterministically.
 */

static void vfs_release_open_file_locks(vfile_t *vf, int gfd);

int g_lookup_errno;

/* Path resolution moved to fs/vfs/path_resolution.c */
extern int g_lookup_errno;

/* ============================================================
 * VFS open / close
 * ============================================================ */

static int relative_path_stays_beneath(const char *relpath) {
    int depth = 0;
    const char *p = relpath;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);
        if (clen == 1 && p[0] == '.') {
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            depth--;
            if (depth < 0) return 0;
        } else {
            depth++;
        }
        p = end;
    }
    return 1;
}

static int vfs_proc_fd_target(const char *path, task_t **task_out, int *fd_out)
{
    const char *prefix = "/proc/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0)
        return 0;

    const char *p = path + prefix_len;
    task_t *task;
    if (strncmp(p, "self/fd/", 8) == 0) {
        task = proc_current();
        p += 8;
    } else {
        if (*p < '0' || *p > '9')
            return 0;
        int pid = 0;
        while (*p >= '0' && *p <= '9') {
            if (pid > 4194304 / 10)
                return -ENOENT;
            pid = pid * 10 + (*p - '0');
            if (pid > 4194304)
                return -ENOENT;
            p++;
        }
        if (strncmp(p, "/fd/", 4) != 0)
            return 0;
        task = proc_find(pid);
        p += 4;
    }
    if (!task || *p < '0' || *p > '9')
        return -ENOENT;

    int fd = 0;
    while (*p >= '0' && *p <= '9') {
        if (fd > (MAX_FILES - 1) / 10)
            return -ENOENT;
        fd = fd * 10 + (*p++ - '0');
        if (fd >= MAX_FILES)
            return -ENOENT;
    }
    if (*p != '\0')
        return -ENOENT;
    *task_out = task;
    *fd_out = fd;
    return 1;
}

static int vfs_proc_fd_path(const char *path, char *out, size_t outsz)
{
    task_t *task = NULL;
    int fd = -1;
    int match = vfs_proc_fd_target(path, &task, &fd);
    if (match <= 0)
        return match;
    int gfd = fdtable_get(task, fd);
    if (gfd < 0)
        return -ENOENT;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -ENOENT;
    size_t len = strlen(vf->path);
    if (len == 0 || len >= outsz) {
        vfs_put_file_ref(gfd, vf);
        return len == 0 ? -ENOENT : -ENAMETOOLONG;
    }
    memcpy(out, vf->path, len + 1);
    vfs_put_file_ref(gfd, vf);
    return 1;
}

int vfs_open(const char *path, int flags, int mode) {
    /* Resolve cwd from current process */
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    /* Check for special device files */
    char resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";
    if (strcmp(root, "/") != 0 && !path_is_beneath(root, resolved)) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, resolved);
        strncpy(resolved, rooted, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    if (vfs_path_normalize_absolute_with_root(resolved, root) < 0)
        return -ENAMETOOLONG;

    if (strcmp(resolved, "/proc/self/exe") == 0) {
        task_t *cur = proc_current();
        const char *exe = cur && cur->exec_path[0] ? cur->exec_path : "/bin/sh";
        return vfs_open(exe, flags, mode);
    }

    char proc_fd_path[MAX_PATH_LEN];
    int proc_fd_match = vfs_proc_fd_path(resolved, proc_fd_path,
                                         sizeof(proc_fd_path));
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0)
        return vfs_open(proc_fd_path, flags, mode);

    /* Find mount point */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) { kdebug("[VFS] open '%s': no mount\n", resolved); return -ENOENT; }

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    vnode_t *vn = vnode_lookup_path(mnt->root, rel);

    if (!vn) {
        if (g_lookup_errno && !(flags & O_CREAT))
            return g_lookup_errno;
        if (!(flags & O_CREAT)) { kdebug("[VFS] open '%s' (rel='%s'): not found, no O_CREAT\n", resolved, rel); return -ENOENT; }
        if (mnt->flags & 1) return -EROFS;
        if (!mnt->root || !mnt->root->ops || !mnt->root->ops->create) { kdebug("[VFS] open '%s': root has no create ops\n", resolved); return -ENOSYS; }

        char parent_path[MAX_PATH_LEN];
        char fname[MAX_NAME_LEN];
        int sr = vfs_path_split_parent_name(rel, parent_path, sizeof(parent_path),
                                            fname, sizeof(fname));
        if (sr < 0)
            return sr;

        vnode_t *parent = vnode_lookup_path(mnt->root, parent_path);
        if (!parent) {
            return g_lookup_errno ? g_lookup_errno : -ENOENT;
        }
        if (parent->type != VFS_FT_DIR) {
            vnode_put(parent);
            return -ENOTDIR;
        }
        if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
            vnode_put(parent);
            return -EACCES;
        }
        if (!parent->ops || !parent->ops->create) {
            kdebug("[VFS] open '%s': parent has no create\n", resolved);
            vnode_put(parent);
            return -ENOSYS;
        }

        int cmode = (mode & S_IFMT) | ((mode & 07777) & ~(cur ? cur->fs.umask : 022));
        int r = parent->ops->create(parent, fname, cmode, &vn);
        vnode_put(parent);
        if (r < 0) { kdebug("[VFS] open '%s': create failed r=%d\n", resolved, r); return r; }
        vfs_dcache_invalidate_all();
        vfs_touch_mtime(vn);
    } else {
        if ((flags & O_DIRECTORY) && vn->type != VFS_FT_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
        int mask = 0;
        if (vfs_should_read(flags)) mask |= R_OK;
        if (vfs_should_write(flags) || (flags & O_TRUNC)) mask |= W_OK;
        if (mask && vfs_vnode_permission(vn, mask) < 0) {
            vnode_put(vn);
            return -EACCES;
        }
        if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
            vnode_put(vn);
            return -EISDIR;
        }
    }

    if ((flags & O_TRUNC) && vn->type == VFS_FT_REGULAR && vn->ops && vn->ops->truncate) {
        int tr = vn->ops->truncate(vn, 0);
        if (tr == 0) {
            page_cache_truncate(vn, 0);
        }
    }

    if (!vn->ops || !vn->ops->open) {
        vnode_put(vn);
        return -ENOSYS;
    }
    vfile_t *vf = vn->ops->open(vn, flags);
    if (!vf) { vnode_put(vn); return -ENOMEM; }
    strncpy(vf->path, resolved, MAX_PATH_LEN - 1);
    vf->path[MAX_PATH_LEN - 1] = '\0';

    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        vnode_put(vn);
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    vnode_put(vn);
    return gfd;
}

int vfs_dirfd_path(int dirfd, char *out, size_t outsz) {
    task_t *cur = proc_current();
    if (dirfd == AT_FDCWD) {
        const char *cwd = cur && cur->fs.cwd[0] ? cur->fs.cwd : "/";
        if (strlen(cwd) >= outsz) return -ENAMETOOLONG;
        strncpy(out, cwd, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }
    int64_t gfd = fdtable_get_current(dirfd);
    if (gfd < 0) return (int)gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode || vf->vnode->type != VFS_FT_DIR) {
        vfs_put_file_ref((int)gfd, vf);
        return -ENOTDIR;
    }
    if (!vf->path[0]) {
        vfs_put_file_ref((int)gfd, vf);
        return -EINVAL;
    }
    if (strlen(vf->path) >= outsz) {
        vfs_put_file_ref((int)gfd, vf);
        return -ENAMETOOLONG;
    }
    strncpy(out, vf->path, outsz - 1);
    out[outsz - 1] = '\0';
    vfs_put_file_ref((int)gfd, vf);
    return 0;
}

int vfs_openat2(int dirfd, const char *path, int flags, int mode, uint64_t resolve) {
    if (!path) return -EFAULT;
    task_t *cur = proc_current();
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";

    char start[MAX_PATH_LEN];
    int r = vfs_dirfd_path(dirfd, start, sizeof(start));
    if (r < 0) return r;

    char logical[MAX_PATH_LEN];
    if (path[0] == '/') {
        if (resolve & RESOLVE_IN_ROOT) {
            if (strcmp(start, "/") == 0)
                snprintf(logical, sizeof(logical), "%s", path);
            else
                snprintf(logical, sizeof(logical), "%s%s", start, path);
        } else {
            snprintf(logical, sizeof(logical), "%s", path);
        }
    } else {
        size_t slen = strlen(start);
        if (slen > 0 && start[slen - 1] == '/')
            snprintf(logical, sizeof(logical), "%s%s", start, path);
        else
            snprintf(logical, sizeof(logical), "%s/%s", start, path);
    }

    if (resolve & RESOLVE_IN_ROOT) {
        if (vfs_path_normalize_absolute_with_root(logical, start) < 0)
            return -ENAMETOOLONG;
    } else {
        if (vfs_path_normalize_absolute(logical) < 0)
            return -ENAMETOOLONG;
    }

    if (strcmp(root, "/") != 0) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(logical, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, logical);
        if (vfs_path_normalize_absolute_with_root(rooted, root) < 0)
            return -ENAMETOOLONG;
        strncpy(logical, rooted, sizeof(logical) - 1);
        logical[sizeof(logical) - 1] = '\0';
    }

    if (resolve & RESOLVE_BENEATH) {
        if (path[0] != '/' && !relative_path_stays_beneath(path))
            return -EXDEV;
    }

    char resolved[MAX_PATH_LEN];
    int lookup_err = 0;
    vnode_t *vn = vnode_lookup_path_openat2(logical, start, root, resolve,
                                            resolved, sizeof(resolved), &lookup_err);

    if (!vn) {
        if (lookup_err == -ENOENT && (flags & O_CREAT)) {
            mount_t *mnt = vfs_find_mount(resolved);
            if (!mnt) return -ENOENT;
            if (mnt->flags & 1) return -EROFS;

            char parent_path[MAX_PATH_LEN];
            char fname[MAX_NAME_LEN];
            if (vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                            fname, sizeof(fname)) < 0)
                return -ENAMETOOLONG;

            vnode_t *parent = vnode_lookup_path(mnt->root,
                                                vfs_strip_mount_prefix(parent_path, mnt));
            if (!parent || parent->type != VFS_FT_DIR) {
                vnode_put(parent);
                return g_lookup_errno ? g_lookup_errno : -ENOENT;
            }
            if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
                vnode_put(parent);
                return -EACCES;
            }
            if (!parent->ops || !parent->ops->create) {
                vnode_put(parent);
                return -ENOSYS;
            }

            int cmode = (mode & S_IFMT) | ((mode & 07777) & ~(cur ? cur->fs.umask : 022));
            int cr = parent->ops->create(parent, fname, cmode, &vn);
            vnode_put(parent);
            if (cr < 0) return cr;
            vfs_dcache_invalidate_all();
            vfs_touch_mtime(vn);
        } else {
            return lookup_err ? lookup_err : -ENOENT;
        }
    }

    if ((flags & O_DIRECTORY) && vn->type != VFS_FT_DIR) {
        vnode_put(vn);
        return -ENOTDIR;
    }
    int mask = 0;
    if (vfs_should_read(flags)) mask |= R_OK;
    if (vfs_should_write(flags) || (flags & O_TRUNC)) mask |= W_OK;
    if (mask && vfs_vnode_permission(vn, mask) < 0) {
        vnode_put(vn);
        return -EACCES;
    }
    if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
        vnode_put(vn);
        return -EISDIR;
    }

    if ((flags & O_TRUNC) && vn->type == VFS_FT_REGULAR && vn->ops && vn->ops->truncate) {
        int tr = vn->ops->truncate(vn, 0);
        if (tr == 0) page_cache_truncate(vn, 0);
    }

    if (!vn->ops || !vn->ops->open) {
        vnode_put(vn);
        return -ENOSYS;
    }
    vfile_t *vf = vn->ops->open(vn, flags);
    if (!vf) { vnode_put(vn); return -ENOMEM; }
    strncpy(vf->path, resolved, MAX_PATH_LEN - 1);
    vf->path[MAX_PATH_LEN - 1] = '\0';

    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        vnode_put(vn);
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    vnode_put(vn);
    return gfd;
}

int vfs_close(int fd) {
    vfile_t *vf = NULL;
    int r = file_close_prepare(fd, &vf);
    if (r < 0) return r;
    if (vf) {
        vnode_t *vn = vf->vnode;
        vfs_release_open_file_locks(vf, fd);
        if (vn && (vn->mode & S_IFMT) == S_IFREG)
            page_cache_writeback_vnode(vn, NULL, NULL);
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        vfile_free(vf);
        vnode_put(vn);
        ktrace_vfs("[VFS] close: gfd=%d done\n", fd);
    }
    return 0;
}

/* ============================================================
 * Directory / File management
 * ============================================================ */

int vfs_mkdir(const char *path, int mode) {
    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;
    vfs_path_trim_trailing_slashes(resolved);
    if (strcmp(resolved, "/") == 0)
        return -EEXIST;

    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt || !mnt->root) return -ENOENT;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    if (!rel[0])
        return -EEXIST;

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(rel, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0)
        return sr;

    vnode_t *parent = vnode_lookup_path(mnt->root, parent_path);
    if (!parent) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    vnode_t *existing = NULL;
    if (parent->ops->lookup) {
        int lookup_r = parent->ops->lookup(parent, name, &existing);
        if (lookup_r == 0 && existing) {
            vnode_put(existing);
            vnode_put(parent);
            return -EEXIST;
        }
    }
    if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    int cmode = (mode & 07777) & ~(cur ? cur->fs.umask : 022);
    int r = parent->ops->mkdir(parent, name, cmode);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_unlink(const char *path) {
    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt || !mnt->root) return -ENOENT;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(rel, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0)
        return sr;

    vnode_t *parent = vnode_lookup_path(mnt->root, parent_path);
    if (!parent) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->unlink) {
        vnode_put(parent);
        return -ENOTDIR;
    }

    vnode_t *victim = NULL;
    if (parent->ops->lookup && parent->ops->lookup(parent, name, &victim) == 0 && victim) {
        int sr = vfs_sticky_may_remove(parent, victim);
        if (sr < 0) {
            vnode_put(victim);
            vnode_put(parent);
            return sr;
        }
    }

    int r = parent->ops->unlink(parent, name);
    if (r == 0)
        vfs_drop_time_meta(victim);
    vnode_put(victim);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_rename_flags(const char *old, const char *newpath, unsigned int flags) {
    if (!old || !newpath) return -EINVAL;
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE))
        return -EINVAL;
    if ((flags & RENAME_NOREPLACE) && (flags & RENAME_EXCHANGE))
        return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";

    char old_resolved[MAX_PATH_LEN];
    char new_resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, old, old_resolved, sizeof(old_resolved));
    if (pr < 0)
        return pr;
    pr = vfs_path_join(cwd, newpath, new_resolved, sizeof(new_resolved));
    if (pr < 0)
        return pr;

    if (strcmp(root, "/") != 0) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(old_resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, old_resolved);
        vfs_path_normalize_absolute_with_root(rooted, root);
        strncpy(old_resolved, rooted, sizeof(old_resolved) - 1);
        old_resolved[sizeof(old_resolved) - 1] = '\0';

        if (strcmp(new_resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, new_resolved);
        vfs_path_normalize_absolute_with_root(rooted, root);
        strncpy(new_resolved, rooted, sizeof(new_resolved) - 1);
        new_resolved[sizeof(new_resolved) - 1] = '\0';
    } else {
        if (vfs_path_normalize_absolute(old_resolved) < 0)
            return -ENAMETOOLONG;
        if (vfs_path_normalize_absolute(new_resolved) < 0)
            return -ENAMETOOLONG;
    }

    char old_parent[MAX_PATH_LEN], old_name[MAX_NAME_LEN];
    char new_parent[MAX_PATH_LEN], new_name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(old_resolved, old_parent, sizeof(old_parent),
                                        old_name, sizeof(old_name));
    if (sr < 0)
        return sr;
    sr = vfs_path_split_parent_name(new_resolved, new_parent, sizeof(new_parent),
                                    new_name, sizeof(new_name));
    if (sr < 0)
        return sr;

    mount_t *old_mnt = vfs_find_mount(old_parent);
    mount_t *new_mnt = vfs_find_mount(new_parent);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV;

    vnode_t *old_dir = vnode_lookup_path(old_mnt->root, vfs_strip_mount_prefix(old_parent, old_mnt));
    vnode_t *new_dir = vnode_lookup_path(new_mnt->root, vfs_strip_mount_prefix(new_parent, new_mnt));
    if (!old_dir || !new_dir) {
        vnode_put(old_dir);
        vnode_put(new_dir);
        return g_lookup_errno ? g_lookup_errno : -ENOENT;
    }
    if (old_dir->type != VFS_FT_DIR || new_dir->type != VFS_FT_DIR) {
        vnode_put(old_dir);
        vnode_put(new_dir);
        return -ENOTDIR;
    }
    if (vfs_vnode_permission(old_dir, W_OK | X_OK) < 0 ||
        vfs_vnode_permission(new_dir, W_OK | X_OK) < 0) {
        vnode_put(old_dir);
        vnode_put(new_dir);
        return -EACCES;
    }
    if (!old_dir->ops || !old_dir->ops->rename) {
        vnode_put(old_dir);
        vnode_put(new_dir);
        return -ENOSYS;
    }

    vnode_t *old_victim = NULL;
    if (old_dir->ops->lookup && old_dir->ops->lookup(old_dir, old_name, &old_victim) == 0 && old_victim) {
        int sr = vfs_sticky_may_remove(old_dir, old_victim);
        vnode_put(old_victim);
        if (sr < 0) {
            vnode_put(old_dir);
            vnode_put(new_dir);
            return sr;
        }
    }

    vnode_t *new_victim = NULL;
    if (new_dir->ops->lookup && new_dir->ops->lookup(new_dir, new_name, &new_victim) == 0 && new_victim) {
        if (flags & RENAME_NOREPLACE) {
            vnode_put(new_victim);
            vnode_put(old_dir);
            vnode_put(new_dir);
            return -EEXIST;
        }
        int sr = vfs_sticky_may_remove(new_dir, new_victim);
        if (sr < 0) {
            vnode_put(new_victim);
            vnode_put(old_dir);
            vnode_put(new_dir);
            return sr;
        }
    }

    int r = old_dir->ops->rename(old_dir, old_name, new_dir, new_name, flags);
    if (r == 0) {
        if (new_victim && !(flags & RENAME_EXCHANGE))
            vfs_drop_time_meta(new_victim);
        vfs_dcache_invalidate(old_dir, old_name);
        vfs_dcache_invalidate(new_dir, new_name);
    }
    vnode_put(new_victim);
    vnode_put(old_dir);
    vnode_put(new_dir);
    return r;
}

int vfs_rename(const char *old, const char *newpath) {
    return vfs_rename_flags(old, newpath, 0);
}

int vfs_rmdir(const char *path) {
    if (!path) return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    char resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0) return pr;

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0) return sr;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt || !mnt->root) return -ENOENT;

    vnode_t *parent = vnode_lookup_path(mnt->root, vfs_strip_mount_prefix(parent_path, mnt));
    if (!parent) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->rmdir) {
        vnode_put(parent);
        return -ENOSYS;
    }

    vnode_t *victim = NULL;
    if (parent->ops->lookup && parent->ops->lookup(parent, name, &victim) == 0 && victim) {
        int sr = vfs_sticky_may_remove(parent, victim);
        if (sr < 0) {
            vnode_put(victim);
            vnode_put(parent);
            return sr;
        }
    }

    int r = parent->ops->rmdir(parent, name);
    if (r == 0)
        vfs_drop_time_meta(victim);
    vnode_put(victim);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

vnode_t *vfs_resolve_no_follow_final(const char *path) {
    if (!path || !*path) return NULL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";

    char resolved[MAX_PATH_LEN];
    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;
    if (strcmp(root, "/") != 0) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, resolved);
        strncpy(resolved, rooted, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    if (vfs_path_normalize_absolute_with_root(resolved, root) < 0)
        return NULL;
    vfs_path_trim_trailing_slashes(resolved);

    if (strcmp(resolved, "/") == 0)
        return vfs_resolve(resolved);

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    if (vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                   name, sizeof(name)) < 0)
        return NULL;

    vnode_t *parent = vfs_resolve_at(parent_path, "/");
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return NULL;
    }

    vnode_t *vn = NULL;
    if (!parent->ops || !parent->ops->lookup) {
        vnode_put(parent);
        return NULL;
    }
    vn = vfs_dcache_lookup(parent, name);
    int r = 0;
    if (!vn) {
        r = parent->ops->lookup(parent, name, &vn);
        if (r == 0 && vn)
            vfs_dcache_insert(parent, name, vn);
    }
    vnode_put(parent);
    if (r < 0 || !vn)
        return NULL;
    return vn;
}

int vfs_statx(const char *path, kstat_t *st, unsigned int mask, int sync_hint) {
    task_t *task = NULL;
    int fd = -1;
    int proc_fd_match = vfs_proc_fd_target(path, &task, &fd);
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0) {
        int gfd = fdtable_get(task, fd);
        return gfd < 0 ? -ENOENT : vfs_fstat(gfd, st);
    }
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (sync_hint == AT_STATX_FORCE_SYNC) {
        page_cache_writeback_vnode(vn, NULL, NULL);
    }
    int r = vfs_vnode_stat(vn, st);
    vnode_put(vn);
    (void)mask;
    return r;
}

int vfs_fstatx(int dirfd, const char *path, kstat_t *st, int flags, unsigned int mask) {
    (void)dirfd;
    int sync_hint = flags & AT_STATX_SYNC_TYPE;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        vnode_t *vn = vfs_resolve_no_follow_final(path);
        if (vn) {
            if (sync_hint == AT_STATX_FORCE_SYNC)
                page_cache_writeback_vnode(vn, NULL, NULL);
            int r = vfs_vnode_stat(vn, st);
            vnode_put(vn);
            (void)mask;
            return r;
        }
    }
    return vfs_statx(path, st, mask, sync_hint);
}

int vfs_stat(const char *path, kstat_t *st) {
    task_t *task = NULL;
    int fd = -1;
    int proc_fd_match = vfs_proc_fd_target(path, &task, &fd);
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0) {
        int gfd = fdtable_get(task, fd);
        return gfd < 0 ? -ENOENT : vfs_fstat(gfd, st);
    }
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    int r = vfs_vnode_stat(vn, st);
    vnode_put(vn);
    return r;
}

int vfs_fstat(int fd, kstat_t *st) {
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = -EBADF;
    if (vf) {
        if (vf->vnode)
            r = vfs_vnode_stat(vf->vnode, st);
        else if (vfs_is_char_device_vfile(vf)) {
            fill_char_kstat(st);
            r = 0;
        } else if (vfs_is_pipe_vfile(vf)) {
            fill_pipe_kstat(st);
            r = 0;
        }
    }
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_fstatat(int dirfd, const char *path, kstat_t *st, int flags) {
    (void)dirfd;
    if (flags & AT_SYMLINK_NOFOLLOW) {
        vnode_t *vn = vfs_resolve_no_follow_final(path);
        if (vn) {
            int r = vfs_vnode_stat(vn, st);
            vnode_put(vn);
            return r;
        }
    }
    return vfs_stat(path, st);
}

int vfs_faccessat(int dirfd, const char *path, int mode) {
    return vfs_faccessat2(dirfd, path, mode, 0);
}

int vfs_faccessat2(int dirfd, const char *path, int mode, int flags) {
    (void)dirfd;
    if (mode & ~(R_OK | W_OK | X_OK)) return -EINVAL;
    if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    if ((!path || path[0] == '\0') && !(flags & AT_EMPTY_PATH)) return -ENOENT;
    mount_t *path_mnt = vfs_find_mount(path);
    if ((mode & W_OK) && path_mnt && (path_mnt->flags & 1))
        return -EROFS;
    vnode_t *vn = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_resolve_no_follow_final(path) : vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    kstat_t st;
    int r = vfs_vnode_stat(vn, &st);
    if (r == 0) {
        if ((mode & W_OK) && vn->mnt && (vn->mnt->flags & 1)) {
            vnode_put(vn);
            return -EROFS;
        }
        task_t *cur = proc_current();
        uint32_t uid = (flags & AT_EACCESS) ? (cur ? (uint32_t)cur->cred.euid : 0)
                                            : (cur ? (uint32_t)cur->cred.uid : 0);
        uint32_t gid = (flags & AT_EACCESS) ? (cur ? (uint32_t)cur->cred.egid : 0)
                                            : (cur ? (uint32_t)cur->cred.gid : 0);
        /* access() uses real uid/gid without capability bypass unless AT_EACCESS */
        if (flags & AT_EACCESS)
            r = vfs_mode_has_perm_ids(st.st_mode, st.st_uid, st.st_gid, uid, gid, mode);
        else
            r = vfs_mode_has_perm_ids_nocap(st.st_mode, st.st_uid, st.st_gid, uid, gid, mode);
        if (r == -EACCES && mode == X_OK &&
            (st.st_mode & S_IFMT) == S_IFREG &&
            !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            int fd = vfs_open(path, O_RDONLY, 0);
            if (fd >= 0) {
                char magic[2];
                int n = vfs_read(fd, magic, sizeof(magic));
                vfs_close(fd);
                if (n >= 2 && magic[0] == '#' && magic[1] == '!')
                    r = 0;
            }
        }
    }
    vnode_put(vn);
    return r;
}

static int vfs_chmod_vnode(vnode_t *vn, int mode) {
    if (!vn) return -ENOENT;
    if (vn->mnt && (vn->mnt->flags & 1))
        return -EROFS;
    if (!vfs_current_owns(vn)) return -EPERM;
    mode &= 07777;
    if (!proc_has_cap(proc_current(), CAP_FOWNER)) {
        kstat_t st;
        if (vfs_vnode_stat(vn, &st) == 0 && !vfs_task_in_group(proc_current(), st.st_gid))
            mode &= ~S_ISGID;
    }
    if (vn->ops && vn->ops->chmod) {
        int r = vn->ops->chmod(vn, mode);
        return r;
    }
    return -EPERM;
}

int vfs_chmodat(int dirfd, const char *path, int mode, int flags) {
    (void)dirfd;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    vnode_t *vn = NULL;
    if ((flags & AT_EMPTY_PATH) && (!path || path[0] == '\0')) {
        return -ENOENT;
    }
    vn = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_resolve_no_follow_final(path) : vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    int r = vfs_chmod_vnode(vn, mode);
    vnode_put(vn);
    return r;
}

int vfs_fchmod(int fd, int mode) {
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = (!vf || !vf->vnode) ? -EBADF : vfs_chmod_vnode(vf->vnode, mode);
    vfs_put_file_ref(fd, vf);
    return r;
}

static int vfs_chown_vnode(vnode_t *vn, int uid, int gid) {
    if (!vn) return -ENOENT;
    if (uid < -1 || gid < -1)
        return -EINVAL;
    task_t *cur = proc_current();
    kstat_t st;
    int r = vfs_vnode_stat(vn, &st);
    if (r < 0) return r;
    if (vn->mnt && (vn->mnt->flags & 1))
        return -EROFS;

    if (!proc_has_cap(cur, CAP_CHOWN)) {
        if (uid != -1 && (uint32_t)uid != st.st_uid)
            return -EPERM;
        if (gid != -1 && !((uint32_t)cur->cred.fsuid == st.st_uid && vfs_task_in_group(cur, (uint32_t)gid)))
            return -EPERM;
    }
    if (vn->ops && vn->ops->chown) {
        int r = vn->ops->chown(vn, uid, gid);
        return r;
    }
    return -EPERM;
}

int vfs_chownat(int dirfd, const char *path, int uid, int gid, int flags) {
    (void)dirfd;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    if ((flags & AT_EMPTY_PATH) && (!path || path[0] == '\0'))
        return -ENOENT;
    vnode_t *vn = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_resolve_no_follow_final(path) : vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    int r = vfs_chown_vnode(vn, uid, gid);
    vnode_put(vn);
    return r;
}

int vfs_fchown(int fd, int uid, int gid) {
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = (!vf || !vf->vnode) ? -EBADF : vfs_chown_vnode(vf->vnode, uid, gid);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_utimensat(int dirfd, const char *path, const uint64_t times[4], int flags) {
    (void)dirfd;
    if (!path) return -EFAULT;
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    vnode_t *vn = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_resolve_no_follow_final(path) : vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (vfs_vnode_permission(vn, W_OK) < 0 && !vfs_current_owns(vn)) {
        vnode_put(vn);
        return -EACCES;
    }
    int r = vfs_set_times(vn, times);
    vnode_put(vn);
    return r;
}

int vfs_futimens(int fd, const uint64_t times[4]) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf || !vf->vnode) {
        vfs_put_file_ref(fd, vf);
        return -EBADF;
    }
    vnode_t *vn = vf->vnode;
    int r;
    if (vfs_vnode_permission(vn, W_OK) < 0 && !vfs_current_owns(vn))
        r = -EACCES;
    else
        r = vfs_set_times(vn, times);
    vfs_put_file_ref(fd, vf);
    return r;
}

static int vfs_readlink_copy_target(const char *target, char *buf, size_t sz) {
    size_t len = strlen(target);
    if (len > sz) len = sz;
    memcpy(buf, target, len);
    return (int)len;
}

int vfs_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    (void)dirfd;
    if (!path || !buf || sz == 0) return -EINVAL;
    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    if (strcmp(resolved, "/proc/self/exe") == 0) {
        task_t *cur = proc_current();
        const char *exe = cur && cur->exec_path[0] ? cur->exec_path : "/bin/sh";
        return vfs_readlink_copy_target(exe, buf, sz);
    }

    if (strcmp(resolved, "/proc/self/cwd") == 0) {
        task_t *cur = proc_current();
        const char *cwd_res = cur ? cur->fs.cwd : "/";
        return vfs_readlink_copy_target(cwd_res, buf, sz);
    }

    task_t *proc_fd_task = NULL;
    int proc_fd = -1;
    int proc_fd_match = vfs_proc_fd_target(resolved, &proc_fd_task, &proc_fd);
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0) {
        int gfd = fdtable_get(proc_fd_task, proc_fd);
        if (gfd < 0)
            return -ENOENT;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (!vf)
            return -EBADF;
        int r = vf->path[0] ? vfs_readlink_copy_target(vf->path, buf, sz) : -ENOENT;
        vfs_put_file_ref(gfd, vf);
        return r;
    }

    /* Handle /proc/<pid>/exe and /proc/<pid>/cwd symlinks */
    if (strncmp(resolved, "/proc/", 6) == 0) {
        const char *p = resolved + 6;
        if (strncmp(p, "self/", 5) == 0) p += 5;
        int pid = 0;
        const char *q = p;
        while (*q >= '0' && *q <= '9') {
            pid = pid * 10 + (*q - '0');
            q++;
        }
        if (pid > 0 && q != p) {
            if (strcmp(q, "/exe") == 0) {
                task_t *t = proc_find(pid);
                const char *exe = t && t->exec_path[0] ? t->exec_path : "/bin/sh";
                return vfs_readlink_copy_target(exe, buf, sz);
            }
            if (strcmp(q, "/cwd") == 0) {
                task_t *t = proc_find(pid);
                const char *cwd_res = t ? t->fs.cwd : "/";
                return vfs_readlink_copy_target(cwd_res, buf, sz);
            }
        }
    }

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0)
        return sr;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return -ENOENT;
    const char *rel = vfs_strip_mount_prefix(parent_path, mnt);
    vnode_t *parent = vnode_lookup_path(mnt->root, rel);
    if (!parent)
        return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    if (vfs_vnode_permission(parent, X_OK) < 0) {
        vnode_put(parent);
        return -EACCES;
    }

    vnode_t *vn = NULL;
    if (parent->ops && parent->ops->lookup) {
        vn = vfs_dcache_lookup(parent, name);
        int r = 0;
        if (!vn) {
            r = parent->ops->lookup(parent, name, &vn);
            if (r == 0 && vn)
                vfs_dcache_insert(parent, name, vn);
        }
        if (r < 0 || !vn) {
            vnode_put(parent);
            return r < 0 ? r : -ENOENT;
        }
    } else {
        vnode_put(parent);
        return -ENOTDIR;
    }
    vnode_put(parent);

    if (vn->type != VFS_FT_SYMLINK || !vn->ops || !vn->ops->readlink) {
        vnode_put(vn);
        return -EINVAL;
    }
    int r = vn->ops->readlink(vn, buf, sz);
    vnode_put(vn);
    return r;
}

int vfs_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -EINVAL;
    vnode_t *target = vfs_resolve(oldpath);
    if (!target) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (target->type == VFS_FT_DIR) {
        vnode_put(target);
        return -EPERM;
    }

    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, newpath, resolved, sizeof(resolved));
    if (pr < 0) {
        vnode_put(target);
        return pr;
    }

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0) {
        vnode_put(target);
        return sr;
    }

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) { vnode_put(target); return -ENOENT; }
    if (target->mnt != mnt) { vnode_put(target); return -EXDEV; }

    vnode_t *parent = vnode_lookup_path(mnt->root, vfs_strip_mount_prefix(parent_path, mnt));
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        vnode_put(target);
        return g_lookup_errno ? g_lookup_errno : -ENOENT;
    }
    if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
        vnode_put(parent);
        vnode_put(target);
        return -EACCES;
    }
    if (!parent->ops || !parent->ops->link) {
        vnode_put(parent);
        vnode_put(target);
        return -ENOSYS;
    }
    int r = parent->ops->link(parent, name, target);
    vnode_put(parent);
    vnode_put(target);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EINVAL;

    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, linkpath, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0)
        return sr;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return -ENOENT;
    const char *rel = vfs_strip_mount_prefix(parent_path, mnt);
    vnode_t *parent = vnode_lookup_path(mnt->root, rel);
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOENT;
    }

    if (!parent->ops || !parent->ops->symlink) {
        vnode_put(parent);
        return -ENOSYS;
    }
    int r = parent->ops->symlink(parent, name, target);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_chdir(const char *path) {
    task_t *cur = proc_current();
    if (!cur) return -EINVAL;

    char canon[MAX_PATH_LEN];
    const char *cwd = cur->fs.cwd[0] ? cur->fs.cwd : "/";
    int pr = vfs_path_join(cwd, path, canon, sizeof(canon));
    if (pr < 0)
        return pr;
    pr = vfs_path_normalize_absolute(canon);
    if (pr < 0)
        return pr;

    vnode_t *vn = vfs_resolve(canon);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (vn->type != VFS_FT_DIR) { vnode_put(vn); return -ENOTDIR; }
    if (vfs_vnode_permission(vn, X_OK) < 0) { vnode_put(vn); return -EACCES; }
    vnode_put(vn);
    strncpy(cur->fs.cwd, canon, MAX_PATH_LEN - 1);
    cur->fs.cwd[MAX_PATH_LEN - 1] = '\0';
    return 0;
}

int vfs_getcwd(char *buf, size_t size) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (size < len) return -ERANGE;
    memcpy(buf, cwd, len);
    return 0;
}

int vfs_poll_events(int fd, short events) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return POLLNVAL;

    int nr = net_poll_events(fd, events);
    if (nr >= 0) {
        vfs_put_file_ref(fd, vf);
        return nr;
    }

    short revents = 0;
    if (vfs_is_pipe_vfile(vf)) {
        revents = pipe_poll_events(vf, events);
        vfs_put_file_ref(fd, vf);
        return revents;
    }

    if (vfs_is_char_device_vfile(vf)) {
        extern int uart_has_input(void);
        if (events & POLLIN) {
            /* fd is a global VFS descriptor, not the process-local stdin fd.
             * TTY readiness must follow the UART receive queue regardless of
             * which global descriptor number was allocated for the stream. */
            if (!devfs_is_tty_vfile(vf) || uart_has_input())
                revents |= POLLIN;
        }
        if (events & POLLOUT)
            revents |= POLLOUT;
        vfs_put_file_ref(fd, vf);
        return revents;
    }

    if (vf->vnode) {
        if (events & POLLIN)
            revents |= POLLIN;
        if (events & POLLOUT)
            revents |= POLLOUT;
        vfs_put_file_ref(fd, vf);
        return revents;
    }

    vfs_put_file_ref(fd, vf);
    return POLLNVAL;
}

int vfs_pipe(int pipefd[2]) {
    return pipe_create(pipefd);
}

/* ============================================================
 * dup / dup3 / fcntl
 * ============================================================ */

typedef struct vfs_fowner {
    int type;
    int pid;
} vfs_fowner_t;

static int vfs_fcntl_getlk(vfile_t *vf, long arg, int owner_kind, uintptr_t owner) {
    if (!arg) return -EFAULT;
    fs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    int r = fs_locks_get(vf, &lk, owner_kind, owner);
    if (r < 0) return r;
    return copy_to_user((void *)arg, &lk, sizeof(lk)) < 0 ? -EFAULT : 0;
}

static int vfs_fcntl_setlk(vfile_t *vf, long arg, int owner_kind, uintptr_t owner, int wait) {
    if (!arg) return -EFAULT;
    fs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    return fs_locks_set(vf, &lk, owner_kind, owner, wait);
}

void vfs_release_process_locks(int pid) {
    fs_locks_release_process(pid);
}

void vfs_release_process_file_locks(int fd, int pid) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return;
    fs_locks_release_process_file(vf, pid);
    vfs_put_file_ref(fd, vf);
}

static void vfs_release_open_file_locks(vfile_t *vf, int gfd __attribute__((unused))) {
    fs_locks_release_file(vf, (uintptr_t)vf);
}

int vfs_flock(int fd, int operation) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return -EBADF;
    int r = fs_flocks_apply(vf, operation);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_fcntl(int fd, int cmd, long arg) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return -EBADF;
    task_t *t = proc_current();
    int owner = t ? t->pid : 0;
#define VFS_FCNTL_RETURN(expr) do { int _vfs_fcntl_r = (expr); vfs_put_file_ref(fd, vf); return _vfs_fcntl_r; } while (0)

    if (cmd == F_GETFL)
        VFS_FCNTL_RETURN(vf->flags);
    if (cmd == F_SETFL) {
        int accmode = vf->flags & O_ACCMODE;
        vf->flags = accmode | ((int)arg & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_DUPFD)
        VFS_FCNTL_RETURN(vfs_dupfd(fd, (int)arg));
    if (cmd == F_DUPFD_CLOEXEC)
        VFS_FCNTL_RETURN(vfs_dupfd(fd, (int)arg));
    if (cmd == F_GETFD || cmd == F_SETFD)
        VFS_FCNTL_RETURN(0);

    if (cmd == F_GETLK)
        VFS_FCNTL_RETURN(vfs_fcntl_getlk(vf, arg, FS_LOCK_OWNER_PID, owner));
    if (cmd == F_SETLK)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_PID, owner, 0));
    if (cmd == F_SETLKW)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_PID, owner, 1));
    if (cmd == F_OFD_GETLK)
        VFS_FCNTL_RETURN(vfs_fcntl_getlk(vf, arg, FS_LOCK_OWNER_OFD, (uintptr_t)vf));
    if (cmd == F_OFD_SETLK)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_OFD, (uintptr_t)vf, 0));
    if (cmd == F_OFD_SETLKW)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_OFD, (uintptr_t)vf, 1));

    if (cmd == F_SETOWN) {
        vf->owner_type = arg < 0 ? F_OWNER_PGRP : F_OWNER_PID;
        vf->owner_pid = arg < 0 ? -(int)arg : (int)arg;
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_GETOWN)
        VFS_FCNTL_RETURN(vf->owner_type == F_OWNER_PGRP ? -vf->owner_pid : vf->owner_pid);
    if (cmd == F_SETOWN_EX) {
        if (!arg) VFS_FCNTL_RETURN(-EFAULT);
        vfs_fowner_t own;
        if (copy_from_user(&own, (void *)arg, sizeof(own)) < 0) VFS_FCNTL_RETURN(-EFAULT);
        if (own.type < F_OWNER_TID || own.type > F_OWNER_PGRP || own.pid < 0)
            VFS_FCNTL_RETURN(-EINVAL);
        vf->owner_type = own.type;
        vf->owner_pid = own.pid;
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_GETOWN_EX) {
        if (!arg) VFS_FCNTL_RETURN(-EFAULT);
        vfs_fowner_t own = {
            .type = vf->owner_type ? vf->owner_type : F_OWNER_PID,
            .pid = vf->owner_pid,
        };
        VFS_FCNTL_RETURN(copy_to_user((void *)arg, &own, sizeof(own)) < 0 ? -EFAULT : 0);
    }
    if (cmd == F_SETSIG) {
        vf->owner_signal = (int)arg;
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_GETSIG)
        VFS_FCNTL_RETURN(vf->owner_signal);
    if (cmd == F_GETOWNER_UIDS) {
        if (!arg) VFS_FCNTL_RETURN(-EFAULT);
        int ids[2] = { t ? t->cred.uid : 0, t ? t->cred.euid : 0 };
        VFS_FCNTL_RETURN(copy_to_user((void *)arg, ids, sizeof(ids)) < 0 ? -EFAULT : 0);
    }

    if (cmd == F_GETPIPE_SZ) {
        if (!vfs_is_pipe_vfile(vf)) VFS_FCNTL_RETURN(-EINVAL);
        VFS_FCNTL_RETURN(pipe_get_size(vf));
    }
    if (cmd == F_SETPIPE_SZ) {
        if (!vfs_is_pipe_vfile(vf)) VFS_FCNTL_RETURN(-EINVAL);
        if (arg <= 0) VFS_FCNTL_RETURN(-EINVAL);
        /* Linux limits unprivileged pipe capacity to 1MB (pipe-max-size).
         * With CAP_SYS_RESOURCE the limit is effectively unlimited. */
        size_t pipe_max = 1048576;
        if (!proc_has_cap(proc_current(), CAP_SYS_RESOURCE) && (size_t)arg > pipe_max)
            VFS_FCNTL_RETURN(-EPERM);
        VFS_FCNTL_RETURN(pipe_set_size(vf, (size_t)arg));
    }

    if (cmd == F_GET_SEALS)
        VFS_FCNTL_RETURN(vf->seals);
    if (cmd == F_ADD_SEALS) {
        if (vf->seals & F_SEAL_SEAL) VFS_FCNTL_RETURN(-EPERM);
        vf->seals |= (int)arg & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
                                 F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
        VFS_FCNTL_RETURN(0);
    }

    if (cmd == F_GET_RW_HINT || cmd == F_GET_FILE_RW_HINT) {
        if (!arg) VFS_FCNTL_RETURN(-EFAULT);
        VFS_FCNTL_RETURN(copy_to_user((void *)arg, &vf->rw_hint, sizeof(vf->rw_hint)) < 0 ? -EFAULT : 0);
    }
    if (cmd == F_SET_RW_HINT || cmd == F_SET_FILE_RW_HINT) {
        if (!arg) VFS_FCNTL_RETURN(-EFAULT);
        uint64_t hint;
        if (copy_from_user(&hint, (void *)arg, sizeof(hint)) < 0) VFS_FCNTL_RETURN(-EFAULT);
        vf->rw_hint = hint;
        VFS_FCNTL_RETURN(0);
    }

    if (cmd == F_SETLEASE) {
        int ltype = (int)arg;
        if (ltype != F_RDLCK && ltype != F_WRLCK)
            VFS_FCNTL_RETURN(-EINVAL);
        int acc = vf->flags & 3;
        if (ltype == F_RDLCK && acc == O_WRONLY)
            VFS_FCNTL_RETURN(-EAGAIN);
        if (ltype == F_WRLCK && acc == O_RDONLY)
            VFS_FCNTL_RETURN(-EAGAIN);
        vf->lease = ltype;
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_GETLEASE)
        VFS_FCNTL_RETURN(vf->lease);
    if (cmd == F_NOTIFY || cmd == F_CANCELLK)
        VFS_FCNTL_RETURN(0);

    VFS_FCNTL_RETURN(-EINVAL);
#undef VFS_FCNTL_RETURN
}

/* Mount operations moved to fs/vfs/mount_ops.c */

/* ============================================================
 * VFS init — set up std streams, root ramfs mount
 * ============================================================ */

void vfs_init(void) {
    file_table_init();
    vfs_mount_table_init();
    if (page_cache_init() < 0)
        kdebug("[VFS] page cache init failed; continuing without it\n");

    {
        mount_t *mnt = vfs_mount_alloc();
        strcpy(mnt->path, "/");
        mnt->type = FS_TYPE_RAMFS;
        strcpy(mnt->dev, "none");
        strcpy(mnt->fstype, "rootfs");
        strcpy(mnt->opts, "rw");
        mnt->root = ramfs_mount(mnt);
        kdebug("[VFS] Initialized (root=ramfs)\n");
    }

    vfs_mkdir("/dev", 0755);
    {
        mount_t *mnt = vfs_mount_alloc();
        strcpy(mnt->path, "/dev");
        mnt->type = FS_TYPE_DEVFS;
        strcpy(mnt->dev, "none");
        strcpy(mnt->fstype, "devtmpfs");
        strcpy(mnt->opts, "rw");
        mnt->root = devfs_mount();
        if (mnt->root) mnt->root->mnt = mnt;
    }

    {
        mount_t *shm_mnt = vfs_mount_alloc();
        strcpy(shm_mnt->path, "/dev/shm");
        shm_mnt->type = FS_TYPE_RAMFS;
        strcpy(shm_mnt->dev, "none");
        strcpy(shm_mnt->fstype, "tmpfs");
        strcpy(shm_mnt->opts, "rw,nosuid,nodev");
        shm_mnt->root = ramfs_mount_empty(shm_mnt);
        if (shm_mnt->root) {
            shm_mnt->root->mnt = shm_mnt;
            kdebug("[VFS] Mounted ramfs at /dev/shm\n");
        }
    }

    /* Install std streams at global fds 0,1,2 */
    file_install_at(STDIN_FILENO, devfs_create_stdio(STDIN_FILENO));
    file_install_at(STDOUT_FILENO, devfs_create_stdio(STDOUT_FILENO));
    file_install_at(STDERR_FILENO, devfs_create_stdio(STDERR_FILENO));

    vfs_mkdir("/tmp", 0755);
    int overlay_err = ramfs_populate_overlay();
    if (overlay_err < 0)
        kerr("[VFS] rootfs overlay population failed: %d\n", overlay_err);

    /* Mount procfs at /proc */
    {
        extern vnode_t *procfs_mount(void);
        vfs_mkdir("/proc", 0755);
        vnode_t *procfs_root = procfs_mount();
        if (procfs_root) {
            mount_t *mnt = vfs_mount_alloc();
            strcpy(mnt->path, "/proc");
            mnt->type = FS_TYPE_PROCFS;
            strcpy(mnt->dev, "proc");
            strcpy(mnt->fstype, "proc");
            strcpy(mnt->opts, "rw");
            mnt->root = procfs_root;
            procfs_root->mnt = mnt;
            kdebug("[VFS] Mounted procfs at /proc\n");
        }
    }

    /* Mount sysfs at /sys */
    {
        extern vnode_t *sysfs_mount(void);
        vfs_mkdir("/sys", 0755);
        vnode_t *sysfs_root = sysfs_mount();
        if (sysfs_root) {
            mount_t *mnt = vfs_mount_alloc();
            strcpy(mnt->path, "/sys");
            mnt->type = FS_TYPE_SYSFS;
            strcpy(mnt->dev, "sysfs");
            strcpy(mnt->fstype, "sysfs");
            strcpy(mnt->opts, "rw");
            mnt->root = sysfs_root;
            sysfs_root->mnt = mnt;
            kdebug("[VFS] Mounted sysfs at /sys\n");
        }
    }
}
