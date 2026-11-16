#include "fs/tty_control.h"
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

#include "fs/quota.h"
#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/file.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/path.h"
#include "fs/vfs/stat_perm.h"
#include "fs/vfs/mutate_state.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "ipc/signalfd.h"
#include "fs/locks.h"
#include "fs/page_cache.h"
#include "fs/inotify.h"
#include "ipc/landlock.h"
#include "ipc/ipc.h"
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

/* Per-task lookup error (vfs_lookup_errno/vfs_set_lookup_errno).  Only the
 * pre-task boot path resolves through the static fallback. */
static int g_lookup_errno_boot;

int vfs_lookup_errno(void)
{
    task_t *t = proc_current();
    return t ? t->lookup_errno : g_lookup_errno_boot;
}

void vfs_set_lookup_errno(int err)
{
    task_t *t = proc_current();
    if (t)
        t->lookup_errno = err;
    else
        g_lookup_errno_boot = err;
}

/* Path resolution moved to fs/vfs/path_resolution.c */

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

int vfs_proc_fd_target(const char *path, task_t **task_out, int *fd_out)
{
    const char *prefix = "/proc/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0)
        return 0;

    const char *p = path + prefix_len;
    task_t *task;
    if (strncmp(p, "self/fd/", 8) == 0) {
        task = proc_get(proc_current());
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
        task = proc_find_get(pid);
        p += 4;
    }
    if (!task)
        return -ENOENT;
    if (*p < '0' || *p > '9') {
        proc_put(task);
        return -ENOENT;
    }

    int fd = 0;
    while (*p >= '0' && *p <= '9') {
        if (fd > (MAX_FILES - 1) / 10)
            goto invalid;
        fd = fd * 10 + (*p++ - '0');
        if (fd >= MAX_FILES)
            goto invalid;
    }
    if (*p != '\0')
        goto invalid;
    *task_out = task;
    *fd_out = fd;
    return 1;

invalid:
    proc_put(task);
    return -ENOENT;
}

/* Called on the canonical path of the same pinned vnode that open will
 * use, before create/truncate/backend open can have any side effects. */
static int vfs_landlock_open(const char *path, int flags, int type)
{
    uint64_t need = 0;
    if (type == VFS_FT_DIR)
        need = LANDLOCK_ACCESS_FS_READ_DIR;
    else {
        if (vfs_should_read(flags)) need |= LANDLOCK_ACCESS_FS_READ_FILE;
        if (vfs_should_write(flags) || (flags & O_TRUNC))
            need |= LANDLOCK_ACCESS_FS_WRITE_FILE;
    }
    return landlock_check_path(path, need);
}

static int vfs_proc_fd_open(const char *path, int flags)
{
    task_t *task = NULL;
    int fd = -1;
    int match = vfs_proc_fd_target(path, &task, &fd);
    if (match <= 0)
        return match;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
    vfile_t *vf = fdtable_get_file_ref(task, fd, NULL);
    if (!vf) {
        proc_put(task);
        return -ENOENT;
    }
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        vfs_put_file(vf);
        proc_put(task);
        return -EEXIST;
    }

    vnode_t *vn = vf->vnode;
    char target_path[MAX_PATH_LEN];
    if (vn)
        vnode_get(vn);
    strncpy(target_path, vf->path, sizeof(target_path) - 1);
    target_path[sizeof(target_path) - 1] = '\0';
    vfs_put_file(vf);
    proc_put(task);
    if (!vn)
        return -ENXIO;
    int lr = vfs_landlock_open(target_path, flags, vn->type);
    if (lr < 0) { vnode_put(vn); return lr; }

    if ((flags & O_DIRECTORY) && vn->type != VFS_FT_DIR) {
        vnode_put(vn);
        return -ENOTDIR;
    }
    int mask = 0;
    if (vfs_should_read(flags))
        mask |= R_OK;
    if (vfs_should_write(flags) || (flags & O_TRUNC))
        mask |= W_OK;
    if (mask) {
        int perm = vfs_vnode_permission(vn, mask);
        if (perm < 0) {
            vnode_put(vn);
            return perm;
        }
    }
    if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
        vnode_put(vn);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && (vn->mode & S_IFMT) == S_IFREG &&
        vn->ops && vn->ops->truncate) {
        int r = vn->ops->truncate(vn, 0);
        if (r < 0) {
            vnode_put(vn);
            return r;
        }
        page_cache_truncate(vn, 0);
    }
    if (!vn->ops || !vn->ops->open) {
        vnode_put(vn);
        return -ENXIO;
    }

    vfile_t *opened = vn->ops->open(vn, flags);
    if (!opened) {
        task_t *cur = proc_current();
        int open_err = cur ? cur->vfs_open_errno : 0;
        if (cur)
            cur->vfs_open_errno = 0;
        vnode_put(vn);
        return open_err ? open_err : -ENOMEM;
    }
    strncpy(opened->path, target_path, MAX_PATH_LEN - 1);
    opened->path[MAX_PATH_LEN - 1] = '\0';
    mount_t *counted_mnt = opened->vnode ? opened->vnode->mnt : NULL;
    if (counted_mnt) {
        vfs_mount_vfile_get(counted_mnt);
        opened->mount_open_counted = 1;
    }
    int opened_fd = fdtable_install_current_vfile(opened, 0);
    if (opened_fd < 0) {
        vfs_put_file(opened);
        vnode_put(vn);
        return -EMFILE;
    }
    vnode_put(vn);
    return opened_fd;
}

static int vfs_open_vnode_impl(struct vnode *vn, int flags)
{
    if (!vn || !vn->ops || !vn->ops->open) {
        if (vn) vnode_put(vn);
        return -ENXIO;
    }
    vfile_t *opened = vn->ops->open(vn, flags);
    if (!opened) {
        task_t *cur = proc_current();
        int error = cur ? cur->vfs_open_errno : 0;
        if (cur) cur->vfs_open_errno = 0;
        vnode_put(vn);
        return error ? error : -ENOMEM;
    }
    strncpy(opened->path, "handle", MAX_PATH_LEN - 1);
    opened->path[MAX_PATH_LEN - 1] = '\0';
    mount_t *counted_mnt = opened->vnode ? opened->vnode->mnt : NULL;
    if (counted_mnt) {
        vfs_mount_vfile_get(counted_mnt);
        opened->mount_open_counted = 1;
    }
    int ofd = fdtable_install_current_vfile(opened, 0);
    if (ofd < 0) {
        vfs_put_file(opened);
        vnode_put(vn);
        return -EMFILE;
    }
    vnode_put(vn);
    return ofd;
}

/* No namespace mutation or filesystem I/O gate may be held across a FIFO
 * peer rendezvous. The descriptor owns the inode; this reference owns the
 * pipe while a signal or concurrent close resolves the pending open. */
static int vfs_finish_open(int fd)
{
    if (fd < 0) return fd;
    vfile_t *vf = fdtable_get_current_file_ref(fd);
    if (!vf) return -EBADF;
    int ret = fifo_finish_open(vf);
    vfs_put_file(vf);
    if (ret < 0) { fdtable_close_current(fd); return ret; }
    return fd;
}

int vfs_open_vnode(struct vnode *vn, int flags)
{
    int ret = mntns_mutation_enter();
    if (ret < 0) {
        vnode_put(vn);
        return ret;
    }
    ret = vfs_open_vnode_impl(vn, flags);
    mntns_mutation_leave();
    return vfs_finish_open(ret);
}

static int vfs_open_impl(const char *path, int flags, int mode) {    /* Resolve cwd from current process */
    task_t *cur = proc_current();
    if (cur)
        cur->vfs_open_errno = 0;
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

    int proc_fd_match = vfs_proc_fd_open(resolved, flags);
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0)
        return proc_fd_match;

    /* Find mount point */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) { kdebug("[VFS] open '%s': no mount\n", resolved); return -ENOENT; }

    char canonical[MAX_PATH_LEN];
    int lookup_err = 0;
    vnode_t *vn = vnode_lookup_path_openat2(resolved, mnt->path, root, 0,
                                            canonical, sizeof(canonical), &lookup_err);
    if (lookup_err) vfs_set_lookup_errno(lookup_err);
    strncpy(resolved, canonical, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
    mnt = vfs_find_mount(resolved);
    if (!mnt) { vnode_put(vn); return -ENOENT; }
    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    int type = vn ? vn->type :
        ((mode & S_IFMT) == S_IFDIR ? VFS_FT_DIR : VFS_FT_REGULAR);
    int lr = vfs_landlock_open(resolved, flags, type);
    if (lr < 0) { vnode_put(vn); return lr; }
    if (!vn) {
        if (vfs_lookup_errno() && !(flags & O_CREAT))
            return vfs_lookup_errno();
        if (!(flags & O_CREAT)) { kdebug("[VFS] open '%s' (rel='%s'): not found, no O_CREAT\n", resolved, rel); return -ENOENT; }
        if (mnt->flags & 1) return -EROFS;
        if (!mnt->root || !mnt->root->ops || !mnt->root->ops->create) { kdebug("[VFS] open '%s': root has no create ops\n", resolved); return -ENOSYS; }

        char parent_path[MAX_PATH_LEN];
        char fname[MAX_NAME_LEN];
        int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                            fname, sizeof(fname));
        if (sr < 0)
            return sr;

        vnode_t *parent = vnode_lookup_path_openat2(parent_path, mnt->path, root, 0,
                                                    canonical, sizeof(canonical), &lookup_err);
        if (!parent) {
            return lookup_err ? lookup_err : -ENOENT;
        }
        if (parent->type != VFS_FT_DIR) {
            vnode_put(parent);
            return -ENOTDIR;
        }
        /* A missing target needs a second walk for its parent. Bind this
         * check to that pinned parent, even if an alias changed meanwhile. */
        sr = vfs_path_join(canonical, fname, resolved, sizeof(resolved));
        if (!sr) sr = vfs_landlock_open(resolved, flags, type);
        if (sr < 0) { vnode_put(parent); return sr; }
        mnt = vfs_find_mount(resolved);
        if (!mnt || (mnt->flags & 1)) {
            vnode_put(parent);
            return mnt ? -EROFS : -ENOENT;
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
        if (cur && quota_check_inode_mnt(parent->mnt,
                                         (uint32_t)cur->cred.euid,
                                         (uint32_t)cur->cred.egid) < 0) {
            vnode_put(parent);
            return -EDQUOT;
        }

        int cmode = (mode & S_IFMT) | ((mode & 07777) & ~(cur ? cur->fs.umask : 022));
        int r = parent->ops->create(parent, fname, cmode, &vn);
        if (r == 0) {
            inotify_vnode_event(parent, fname, IN_CREATE);
            a20_fs_notify(parent, A20_EVENT_FS_CREATE, fname, 0, 0);
            vfs_dcache_invalidate(parent, fname);
            vfs_dcache_insert(parent, fname, vn);
            quota_account_inode_create(vn);
        }
        vnode_put(parent);
        if (r < 0) { kdebug("[VFS] open '%s': create failed r=%d\n", resolved, r); return r; }
        vfs_touch_mtime(vn);
    } else {
        if ((flags & O_DIRECTORY) && vn->type != VFS_FT_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
        int mask = 0;
        if (vfs_should_read(flags)) mask |= R_OK;
        if (vfs_should_write(flags) || (flags & O_TRUNC)) mask |= W_OK;
        if (mask) {
            int perm = vfs_vnode_permission(vn, mask);
            if (perm < 0) {
                vnode_put(vn);
                return perm;
            }
        }
        if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
            vnode_put(vn);
            return -EISDIR;
        }
    }

    if ((flags & O_TRUNC) && (vn->mode & S_IFMT) == S_IFREG && vn->ops && vn->ops->truncate) {
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
    if (!vf) {
        int open_err = cur ? cur->vfs_open_errno : 0;
        if (cur)
            cur->vfs_open_errno = 0;
        vnode_put(vn);
        return open_err ? open_err : -ENOMEM;
    }
    strncpy(vf->path, resolved, MAX_PATH_LEN - 1);
    vf->path[MAX_PATH_LEN - 1] = '\0';

    mount_t *counted_mnt = vn->mnt;
    if (counted_mnt) {
        vfs_mount_vfile_get(counted_mnt);
        vf->mount_open_counted = 1;
    }
    int ofd = fdtable_install_current_vfile(vf, flags & O_CLOEXEC);
    if (ofd < 0) {
        vnode_put(vn);
        vfs_put_file(vf);
        return -EMFILE;
    }
    vnode_put(vn);
    return ofd;
}

int vfs_open(const char *path, int flags, int mode)
{
    int ret = mntns_mutation_enter();
    if (ret < 0)
        return ret;
    ret = vfs_open_impl(path, flags, mode);
    mntns_mutation_leave();
    return vfs_finish_open(ret);
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
    vfile_t *vf = fdtable_get_current_file_ref(dirfd);
    if (!vf) return -EBADF;
    if (!vf->vnode || vf->vnode->type != VFS_FT_DIR) {
        vfs_put_file(vf);
        return -ENOTDIR;
    }
    if (!vf->path[0]) {
        vfs_put_file(vf);
        return -EINVAL;
    }
    if (strlen(vf->path) >= outsz) {
        vfs_put_file(vf);
        return -ENAMETOOLONG;
    }
    strncpy(out, vf->path, outsz - 1);
    out[outsz - 1] = '\0';
    vfs_put_file(vf);
    return 0;
}

static int vfs_openat2_impl(int dirfd, const char *path, int flags, int mode, uint64_t resolve) {
    if (!path) return -EFAULT;
    task_t *cur = proc_current();
    if (cur)
        cur->vfs_open_errno = 0;
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

    if (strcmp(root, "/") != 0 && !path_is_beneath(root, logical)) {
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
    int type = vn ? vn->type :
        ((mode & S_IFMT) == S_IFDIR ? VFS_FT_DIR : VFS_FT_REGULAR);
    int lr = vfs_landlock_open(resolved, flags, type);
    if (lr < 0) { vnode_put(vn); return lr; }

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

            char parent_canonical[MAX_PATH_LEN];
            vnode_t *parent = vnode_lookup_path_openat2(parent_path, start, root, resolve,
                                                        parent_canonical, sizeof(parent_canonical), &lookup_err);
            if (!parent || parent->type != VFS_FT_DIR) {
                vnode_put(parent);
                return lookup_err ? lookup_err : -ENOTDIR;
            }
            int cr = vfs_path_join(parent_canonical, fname, resolved, sizeof(resolved));
            if (!cr) cr = vfs_landlock_open(resolved, flags, type);
            if (cr < 0) { vnode_put(parent); return cr; }
            mnt = vfs_find_mount(resolved);
            if (!mnt || (mnt->flags & 1)) {
                vnode_put(parent);
                return mnt ? -EROFS : -ENOENT;
            }
            if (vfs_vnode_permission(parent, W_OK | X_OK) < 0) {
                vnode_put(parent);
                return -EACCES;
            }
            if (!parent->ops || !parent->ops->create) {
                vnode_put(parent);
                return -ENOSYS;
            }
            if (cur && quota_check_inode_mnt(parent->mnt,
                                             (uint32_t)cur->cred.euid,
                                             (uint32_t)cur->cred.egid) < 0) {
                vnode_put(parent);
                return -EDQUOT;
            }

            int cmode = (mode & S_IFMT) | ((mode & 07777) & ~(cur ? cur->fs.umask : 022));
            cr = parent->ops->create(parent, fname, cmode, &vn);
            if (cr < 0) {
                vnode_put(parent);
                return cr;
            }
            quota_account_inode_create(vn);
            inotify_vnode_event(parent, fname, IN_CREATE);
            a20_fs_notify(parent, A20_EVENT_FS_CREATE, fname, 0, 0);
            vfs_dcache_invalidate(parent, fname);
            vfs_dcache_insert(parent, fname, vn);
            /* Keep the vnode returned by create canonical for subsequent
             * opens.  Shared-memory users commonly open the same new inode
             * once read-write and once read-only before unlinking it; making
             * a second vnode would split cached size and mmap page state. */
            vnode_put(parent);
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
    if (mask) {
        int perm = vfs_vnode_permission(vn, mask);
        if (perm < 0) {
            vnode_put(vn);
            return perm;
        }
    }
    if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
        vnode_put(vn);
        return -EISDIR;
    }

    if ((flags & O_TRUNC) && (vn->mode & S_IFMT) == S_IFREG && vn->ops && vn->ops->truncate) {
        int tr = vn->ops->truncate(vn, 0);
        if (tr == 0) page_cache_truncate(vn, 0);
    }

    if (!vn->ops || !vn->ops->open) {
        vnode_put(vn);
        return -ENOSYS;
    }
    vfile_t *vf = vn->ops->open(vn, flags);
    if (!vf) {
        int open_err = cur ? cur->vfs_open_errno : 0;
        if (cur)
            cur->vfs_open_errno = 0;
        vnode_put(vn);
        return open_err ? open_err : -ENOMEM;
    }
    strncpy(vf->path, resolved, MAX_PATH_LEN - 1);
    vf->path[MAX_PATH_LEN - 1] = '\0';

    int ofd = fdtable_install_current_vfile(vf, flags & O_CLOEXEC);
    if (ofd < 0) {
        vnode_put(vn);
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    vnode_put(vn);
    return ofd;
}

int vfs_openat2(int dirfd, const char *path, int flags, int mode, uint64_t resolve)
{
    int ret = mntns_mutation_enter();
    if (ret < 0) return ret;
    ret = vfs_openat2_impl(dirfd, path, flags, mode, resolve);
    mntns_mutation_leave();
    return vfs_finish_open(ret);
}

/* mknod creates directory metadata without opening an endpoint. In
 * particular mkfifo must neither rendezvous nor acquire read/write access. */
int vfs_mknod(const char *path, int mode)
{
    int ret = mntns_mutation_enter();
    if (ret < 0) return ret;
    vnode_t *parent = NULL, *vn = NULL;
    task_t *task = proc_current();
    char resolved[MAX_PATH_LEN], parent_path[MAX_PATH_LEN], name[MAX_NAME_LEN];
    ret = vfs_path_join(task ? task->fs.cwd : "/", path, resolved, sizeof(resolved));
    if (ret < 0) goto out;
    ret = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret < 0) goto out;
    const char *root = task && task->fs.root_path[0] ? task->fs.root_path : "/";
    char canonical[MAX_PATH_LEN];
    int lookup_error = 0;
    parent = vnode_lookup_path_openat2(parent_path, root, root, 0,
                                       canonical, sizeof(canonical), &lookup_error);
    if (!parent) { ret = lookup_error ? lookup_error : -ENOENT; goto out; }
    if (parent->type != VFS_FT_DIR) { ret = -ENOTDIR; goto out; }
    ret = vfs_path_join(canonical, name, resolved, sizeof(resolved));
    if (ret < 0) goto out;
    if (parent->mnt && (parent->mnt->flags & VFS_MOUNT_RDONLY)) { ret = -EROFS; goto out; }
    if ((mode & S_IFMT) == S_IFIFO && (!parent->mnt ||
        (parent->mnt->type != FS_TYPE_RAMFS && parent->mnt->type != FS_TYPE_EXT4))) {
        ret = -EOPNOTSUPP; goto out;
    }
    ret = vfs_vnode_permission(parent, W_OK | X_OK);
    if (ret < 0) goto out;
    ret = landlock_check_path(resolved, (mode & S_IFMT) == S_IFIFO ?
        LANDLOCK_ACCESS_FS_MAKE_FIFO : LANDLOCK_ACCESS_FS_MAKE_REG);
    if (ret < 0) goto out;
    if (!parent->ops || !parent->ops->create) { ret = -EOPNOTSUPP; goto out; }
    if (task && quota_check_inode_mnt(parent->mnt, task->cred.euid, task->cred.egid) < 0) {
        ret = -EDQUOT; goto out;
    }
    mode = (mode & S_IFMT) | ((mode & 07777) & ~(task ? task->fs.umask : 022));
    ret = parent->ops->create(parent, name, mode, &vn);
    if (!ret) {
        vfs_dcache_invalidate(parent, name);
        vfs_dcache_insert(parent, name, vn);
        quota_account_inode_create(vn);
        inotify_vnode_event(parent, name, IN_CREATE);
        a20_fs_notify(parent, A20_EVENT_FS_CREATE, name, 0, 0);
    }
out:
    vnode_put(vn);
    vnode_put(parent);
    mntns_mutation_leave();
    return ret;
}

/* Runs when the last reference to an open file goes away: fire EventQ
 * destroy hooks (watches key on the vfile pointer), release file locks,
 * run the close op, free the vfile, drop the vnode. */
int vfs_finalize_closed_vfile(vfile_t *vf)
{
    if (!vf)
        return 0;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] finalize_enter vf=%lu refs=%d mounted=%u deferred=%u token_cap=%u\n",
          (unsigned long)vf->identity, vfile_ref_read(vf),
          vf->mount_open_counted, vf->shutdown_deferred_close,
          vf->close_token_capable);
#endif
    vnode_t *vn = vf->vnode;
    unsigned long identity = (unsigned long)vf->identity;
    (void)identity; /* ktrace_vfs compiles away in non-trace builds. */
    int close_error = 0;
    if (!vf->finalize_started) {
        vf->finalize_started = 1;
        a20_eventq_on_vfile_destroy(vf);
        vfs_release_open_file_locks(vf, 0);
    }
    if (!vf->backend_close_done)
        close_error = vfile_backend_close(vf);
    if (close_error < 0 && !vf->backend_close_done) {
        /* fdtable already consumed the descriptor. Retain an internal vfile
         * reference and its mount ownership so system shutdown can retry a
         * known pre-CLOSE flush failure or fail closed on an ambiguous RPC. */
        vf->shutdown_deferred_close = 1;
        vfile_ref_init(vf, 1);
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
        kinfo("[UFS-CT-TRACE] finalize_deferred vf=%lu err=%d refs=%d mounted=%u state=%u\n",
              (unsigned long)vf->identity, close_error,
              vfile_ref_read(vf), vf->mount_open_counted,
              vf->backend_close_state);
#endif
        vfile_finalize_done();
        return close_error;
    }
    /* A backend may retire local state after a proven peer death while
     * returning EIO. Preserve the error and release ownership exactly once. */
    vf->backend_close_done = 1;
    if (vf->mount_open_counted && vn && vn->mnt) {
        vf->mount_open_counted = 0;
        vfs_mount_vfile_put(vn->mnt);
    }
    ktrace_vfs("[VFS] close: file=%p identity=%lu done err=%d\n",
               (void *)vf, identity, close_error);
    vfile_free(vf);
    vnode_put(vn);
    vfile_finalize_done();
    return close_error;
}

/*
 * Release a descriptor slot.
 *
 * Dropping the slot's vfile reference is not enough: files->fd[fd] stays
 * non-NULL, its open_mask bit stays set, and next_fd never rewinds, so
 * fdtable_install_vfile() eventually walks 0..MAX_FILES and returns EMFILE
 * against a table that is entirely empty.  Because install takes place in the
 * *calling task's* table, every internal vfs_open()/vfs_close() pair burned
 * one of that task's slots -- and the stat() shebang probe (see
 * path_is_shebang_script) does exactly such a pair on every stat of a
 * non-executable regular file, which is most of what a desktop session
 * touches.  xfce4-panel exhausted all MAX_FILES slots during its own startup
 * and GLib then hit EMFILE from g_wakeup_new().
 *
 * fdtable_close() releases the slot and does the teardown that used to live
 * here -- the vfile reference, the envelope registry entry, the per-process
 * file locks, and vfs_finalize_closed_vfile() (ops->close, vnode_put, the
 * destroy event).  With no current task the descriptor belongs to the boot
 * table, which has no owning task; fdtable_close_active() reaches that table
 * through fdtable_close_files(), so a descriptor opened before the first task
 * existed is released the same way.  Every boot-time caller in main.c,
 * driver_manager.c and elf.c stops using the descriptor immediately after the
 * close, so none of them depends on the slot staying open.
 */
int vfs_close(int fd) {
    return fdtable_close_active(fd);
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

    vnode_t *parent = vnode_lookup_path(mnt, parent_path);
    if (!parent) return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
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
    if (cur && quota_check_inode_mnt(mnt, (uint32_t)cur->cred.euid,
                                     (uint32_t)cur->cred.egid) < 0) {
        vnode_put(parent);
        return -EDQUOT;
    }
    int r = parent->ops->mkdir(parent, name, cmode);
    if (r == 0) {
        inotify_vnode_event(parent, name, IN_CREATE | IN_ISDIR);
        a20_fs_notify(parent, A20_EVENT_FS_CREATE, name, 0, 0);
        vfs_dcache_invalidate(parent, name);
        quota_account_inode_ids(mnt, cur ? (uint32_t)cur->cred.euid : 0,
                                cur ? (uint32_t)cur->cred.egid : 0);
    }
    vnode_put(parent);
    return r;
}

/* ---------------------------------------------------------------------------
 * Rust-owned directory-mutation orchestration (VFS_MUTATE_IMPL=rust)
 *
 * The engine owns the decision order of unlink/rmdir/rename; these callbacks
 * are the mechanical half -- the path plumbing (the Rust-owned string layer
 * plus the mount table), the vnode references, the filesystem
 * lookup/unlink/rmdir/rename calls, the permission and sticky checks, and the
 * notification/quota/dcache side effects.
 * ------------------------------------------------------------------------- */

static int mut_join(const char *cwd, const char *path, char *out, size_t outsz)
{
    return vfs_path_join(cwd, path, out, outsz);
}
static int mut_split(const char *path, char *parent, size_t psz, char *name,
                     size_t nsz, size_t max_path_len)
{
    (void)max_path_len; /* path.c bounds with its own MAX_PATH_LEN */
    return vfs_path_split_parent_name(path, parent, psz, name, nsz);
}
static int mut_norm(char *path, const char *root, size_t max_path_len)
{
    (void)max_path_len;
    return vfs_path_normalize_absolute_with_root(path, root);
}
static int mut_beneath(const char *s, const char *r) { return path_is_beneath(s, r); }
static void *mut_find_mount(const char *p) { return vfs_find_mount(p); }
static const char *mut_strip(const char *p, const void *m)
{
    return vfs_strip_mount_prefix(p, (const mount_t *)m);
}
static void *mut_mnt_root(const void *m) { return ((const mount_t *)m)->root; }
static int mut_mnt_type(const void *m) { return ((const mount_t *)m)->type; }
static void *mut_lookup_path(void *m, const char *r)
{
    return vnode_lookup_path((mount_t *)m, r);
}
static int mut_errno(void) { return vfs_lookup_errno(); }
static void mut_put(void *v) { vnode_put((vnode_t *)v); }
static int mut_type(const void *v) { return ((const vnode_t *)v)->type; }
static uint64_t mut_ino(const void *v) { return ((const vnode_t *)v)->ino; }
static size_t mut_size(const void *v) { return ((const vnode_t *)v)->size; }
static void *mut_mnt(const void *v) { return ((const vnode_t *)v)->mnt; }
static int mut_has_lookup(const void *d)
{
    const vnode_t *v = (const vnode_t *)d;
    return v->ops && v->ops->lookup ? 1 : 0;
}
static int mut_lookup(void *d, const char *n, void **o)
{
    vnode_t *dir = (vnode_t *)d;
    vnode_t *v = NULL;
    int r = dir->ops->lookup(dir, n, &v);
    *o = v;
    return r;
}
static int mut_has_unlink(const void *d)
{
    const vnode_t *v = (const vnode_t *)d;
    return v->ops && v->ops->unlink ? 1 : 0;
}
static int mut_has_rmdir(const void *d)
{
    const vnode_t *v = (const vnode_t *)d;
    return v->ops && v->ops->rmdir ? 1 : 0;
}
static int mut_has_rename(const void *d)
{
    const vnode_t *v = (const vnode_t *)d;
    return v->ops && v->ops->rename ? 1 : 0;
}
static int mut_do_unlink(void *d, const char *n)
{
    return ((vnode_t *)d)->ops->unlink((vnode_t *)d, n);
}
static int mut_do_rmdir(void *d, const char *n)
{
    return ((vnode_t *)d)->ops->rmdir((vnode_t *)d, n);
}
static int mut_do_rename(void *od, const char *on, void *nd, const char *nn, uint32_t f)
{
    return ((vnode_t *)od)->ops->rename((vnode_t *)od, on, (vnode_t *)nd, nn, f);
}
static int mut_perm(void *v, int mask) { return vfs_vnode_permission((vnode_t *)v, mask); }
static int mut_sticky(void *d, void *victim)
{
    return vfs_sticky_may_remove((vnode_t *)d, (vnode_t *)victim);
}
static void mut_notify_delete(void *d, const char *n, int is_dir)
{
    inotify_vnode_event((vnode_t *)d, n, is_dir ? (IN_DELETE | IN_ISDIR) : IN_DELETE);
    a20_fs_notify(d, A20_EVENT_FS_DELETE, n, 0, 0);
}
static void mut_notify_rename(void *od, const char *on, void *nd, const char *nn)
{
    inotify_vnode_event((vnode_t *)od, on, IN_MOVED_FROM);
    a20_fs_notify(od, A20_EVENT_FS_RENAME, on, 0, 0);
    inotify_vnode_event((vnode_t *)nd, nn, IN_MOVED_TO);
    a20_fs_notify(nd, A20_EVENT_FS_RENAME, nn, 0, 0);
}
static void mut_notify_self(void *v) { inotify_vnode_event((vnode_t *)v, NULL, IN_DELETE_SELF); }
static void mut_quota(void *v, size_t sz) { quota_account_inode_remove((vnode_t *)v, sz); }
static void mut_dc_inv(void *d, const char *n) { vfs_dcache_invalidate((vnode_t *)d, n); }
static void mut_dc_inv_all(void) { vfs_dcache_invalidate_all(); }

static const struct vfs_mutate_ops g_mutate_ops = {
    .join = mut_join, .split_parent_name = mut_split, .normalize_with_root = mut_norm,
    .is_beneath = mut_beneath, .find_mount = mut_find_mount, .strip_mount_prefix = mut_strip,
    .mnt_root = mut_mnt_root, .mnt_type = mut_mnt_type, .lookup_path = mut_lookup_path,
    .lookup_errno = mut_errno, .vnode_put = mut_put, .vn_type = mut_type, .vn_ino = mut_ino,
    .vn_size = mut_size, .vn_mnt = mut_mnt, .has_lookup = mut_has_lookup, .lookup = mut_lookup,
    .has_unlink = mut_has_unlink, .has_rmdir = mut_has_rmdir, .has_rename = mut_has_rename,
    .do_unlink = mut_do_unlink, .do_rmdir = mut_do_rmdir, .do_rename = mut_do_rename,
    .permission = mut_perm, .sticky_may_remove = mut_sticky, .notify_delete = mut_notify_delete,
    .notify_rename = mut_notify_rename, .notify_self_delete = mut_notify_self,
    .quota_remove = mut_quota, .dcache_invalidate = mut_dc_inv,
    .dcache_invalidate_all = mut_dc_inv_all,
    .max_path_len = MAX_PATH_LEN, .max_name_len = MAX_NAME_LEN,
};


int vfs_unlink(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    return a20_vfs_unlink(&g_mutate_ops, cwd, path);
}

int vfs_rename_flags(const char *old, const char *newpath, unsigned int flags) {
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";
    return a20_vfs_rename_flags(&g_mutate_ops, cwd, root, old, newpath, flags);
}

int vfs_rename(const char *old, const char *newpath) {
    return vfs_rename_flags(old, newpath, 0);
}

int vfs_rmdir(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    return a20_vfs_rmdir(&g_mutate_ops, cwd, path);
}

vnode_t *vfs_resolve_no_follow_final(const char *path) {
    if (!path || !*path) return NULL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";
    const char *root = cur && cur->fs.root_path[0] ? cur->fs.root_path : "/";

    char resolved[MAX_PATH_LEN];
    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;
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


static int vfs_readlink_copy_target(const char *target, char *buf, size_t sz) {
    size_t len = strlen(target);
    if (len > sz) len = sz;
    memcpy(buf, target, len);
    return (int)len;
}

/*
 * Paths kept by vfiles and exec are global VFS paths.  A task's cwd and
 * procfs symlink results, however, are paths in that task's chroot-visible
 * namespace.  Translate only on the visibility boundary; callers doing VFS
 * lookup continue to use the physical path.
 */
static const char *vfs_chroot_visible_path(task_t *task, const char *physical,
                                           char *visible, size_t visible_sz) {
    const char *root =
        task && task->fs.root_path[0] ? task->fs.root_path : "/";
    if (!physical || !visible || visible_sz == 0)
        return NULL;
    if (strcmp(root, "/") == 0 || !path_is_beneath(root, physical))
        return physical;
    const char *relative = physical + strlen(root);
    if (*relative == '\0')
        relative = "/";
    if (strlen(relative) >= visible_sz)
        return NULL;
    strcpy(visible, relative);
    return visible;
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

    char visible_path[MAX_PATH_LEN];
    const char *visible =
        vfs_chroot_visible_path(cur, resolved, visible_path,
                                sizeof(visible_path));
    if (!visible)
        return -ENAMETOOLONG;

    if (strcmp(visible, "/proc/self/exe") == 0) {
        const char *exe = cur && cur->exec_path[0] ? cur->exec_path : "/bin/sh";
        char visible_exe[MAX_PATH_LEN];
        const char *target =
            vfs_chroot_visible_path(cur, exe, visible_exe,
                                    sizeof(visible_exe));
        return target ? vfs_readlink_copy_target(target, buf, sz)
                      : -ENAMETOOLONG;
    }

    if (strcmp(visible, "/proc/self/cwd") == 0) {
        const char *cwd_res = cur ? cur->fs.cwd : "/";
        return vfs_readlink_copy_target(cwd_res, buf, sz);
    }

    task_t *proc_fd_task = NULL;
    int proc_fd = -1;
    int proc_fd_match = vfs_proc_fd_target(resolved, &proc_fd_task, &proc_fd);
    if (proc_fd_match < 0)
        return proc_fd_match;
    if (proc_fd_match > 0) {
        if (!proc_task_may_access(proc_current(), proc_fd_task)) {
            proc_put(proc_fd_task);
            return -EACCES;
        }
        vfile_t *vf = fdtable_get_file_ref(proc_fd_task, proc_fd, NULL);
        if (!vf) {
            proc_put(proc_fd_task);
            return -ENOENT;
        }
        char visible_fd[MAX_PATH_LEN];
        const char *target =
            vf->path[0]
                ? vfs_chroot_visible_path(cur, vf->path, visible_fd,
                                          sizeof(visible_fd))
                : NULL;
        int r = target ? vfs_readlink_copy_target(target, buf, sz) : -ENOENT;
        vfs_put_file(vf);
        proc_put(proc_fd_task);
        return r;
    }

    /* Handle /proc/<pid>/exe and /proc/<pid>/cwd symlinks */
    if (strncmp(visible, "/proc/", 6) == 0) {
        const char *p = visible + 6;
        if (strncmp(p, "self/", 5) == 0) p += 5;
        int pid = 0;
        const char *q = p;
        while (*q >= '0' && *q <= '9') {
            pid = pid * 10 + (*q - '0');
            q++;
        }
        if (pid > 0 && q != p) {
            if (strcmp(q, "/exe") == 0) {
                task_t *t = proc_find_get(pid);
                const char *exe = t && t->exec_path[0] ? t->exec_path : "/bin/sh";
                char visible_exe[MAX_PATH_LEN];
                const char *target =
                    vfs_chroot_visible_path(cur, exe, visible_exe,
                                            sizeof(visible_exe));
                int r = target ? vfs_readlink_copy_target(target, buf, sz)
                               : -ENAMETOOLONG;
                proc_put(t);
                return r;
            }
            if (strcmp(q, "/cwd") == 0) {
                task_t *t = proc_find_get(pid);
                const char *cwd_res = t ? t->fs.cwd : "/";
                int r = vfs_readlink_copy_target(cwd_res, buf, sz);
                proc_put(t);
                return r;
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
    vnode_t *parent = vnode_lookup_path(mnt, rel);
    if (!parent)
        return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
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

    /* Allow readlink() on a vnode whose ops implement it even when the
     * vnode is a directory: A20OS sysfs exposes /sys/dev/char/<maj>:<min>
     * as a real directory (libdrm walks device/drm beneath it) that also
     * answers readlink() with the class-device path.  On Linux the two are
     * symlinks to the same /sys/devices path; this keeps both views
     * consistent so libudev's udev_device_new_from_devnum() and an
     * enumerate from /sys/class produce the same syspath. */
    if (!vn->ops || !vn->ops->readlink) {
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
    if (!target) return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
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

    vnode_t *parent = vnode_lookup_path(mnt, vfs_strip_mount_prefix(parent_path, mnt));
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        vnode_put(target);
        return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
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
    if (r == 0) {
        inotify_vnode_event(parent, name, IN_CREATE);
        a20_fs_notify(parent, A20_EVENT_FS_CREATE, name, 0, 0);
        vfs_dcache_invalidate(parent, name);
    }
    vnode_put(parent);
    vnode_put(target);
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
    vnode_t *parent = vnode_lookup_path(mnt, rel);
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOENT;
    }

    if (!parent->ops || !parent->ops->symlink) {
        vnode_put(parent);
        return -ENOSYS;
    }
    int r = parent->ops->symlink(parent, name, target);
    if (r == 0) {
        inotify_vnode_event(parent, name, IN_CREATE);
        a20_fs_notify(parent, A20_EVENT_FS_CREATE, name, 0, 0);
        vfs_dcache_invalidate(parent, name);
        quota_account_inode_ids(mnt, cur ? (uint32_t)cur->cred.euid : 0,
                                cur ? (uint32_t)cur->cred.egid : 0);
    }
    vnode_put(parent);
    return r;
}

/* ---- per-process root and cwd references -----------------------------
 *
 * The strings in proc_fs_context_t are the flattened spellings; these four
 * helpers are the only places that move the object references they are
 * derived from, so the pair cannot drift apart.  Every transition takes the
 * new reference before releasing the old one, which matters for chdir(".")
 * and for re-pivoting onto the same directory. */

/* Move @t's root to the (mount, vnode) pair, @root_path being the global
 * path that spells it.  Called by chroot(2), pivot_root(2) and the
 * namespace-apply path. */
void vfs_task_root_set(task_t *t, mount_t *mnt, vnode_t *vn,
                       const char *root_path) {
    if (!t) return;
    if (vn) vnode_get(vn);
    vfs_mount_root_get(mnt);
    if (t->fs.root_vn) vnode_put(t->fs.root_vn);
    if (t->fs.root_mnt) vfs_mount_root_put(t->fs.root_mnt);
    t->fs.root_vn = vn;
    t->fs.root_mnt = mnt;
    if (root_path) {
        /* Match procfs's cross-task root-path snapshot. No vnode puts or
         * mount-table operations may run under this short string lock. */
        uint64_t flags = spin_lock_irqsave(&t->park_lock);
        strncpy(t->fs.root_path, root_path, MAX_PATH_LEN - 1);
        t->fs.root_path[MAX_PATH_LEN - 1] = '\0';
        spin_unlock_irqrestore(&t->park_lock, flags);
    }
    /* Linux resets the cwd to the new root on chroot; pivot_root sets it
     * explicitly afterwards.  Keeping cwd root-relative, "/" is correct for
     * both. */
    strncpy(t->fs.cwd, "/", MAX_PATH_LEN - 1);
    t->fs.cwd[MAX_PATH_LEN - 1] = '\0';
    if (t->fs.cwd_vn) vnode_put(t->fs.cwd_vn);
    t->fs.cwd_vn = vn;
    if (vn) vnode_get(vn);
    if (mnt) vfs_mount_cwd_get(mnt);
}

/* Move @t's cwd to @vn, @visible being its root-relative spelling. */
void vfs_task_cwd_set(task_t *t, vnode_t *vn, const char *visible) {
    if (!t) return;
    if (vn) vnode_get(vn);
    mount_t *new_mnt = vn ? vn->mnt : NULL;
    if (new_mnt) vfs_mount_cwd_get(new_mnt);
    mount_t *old_mnt = t->fs.cwd_vn ? t->fs.cwd_vn->mnt : NULL;
    if (t->fs.cwd_vn) vnode_put(t->fs.cwd_vn);
    if (old_mnt) vfs_mount_cwd_put(old_mnt);
    t->fs.cwd_vn = vn;
    if (visible) {
        strncpy(t->fs.cwd, visible, MAX_PATH_LEN - 1);
        t->fs.cwd[MAX_PATH_LEN - 1] = '\0';
    }
}

/* Copy @src's root/cwd references into @dst, taking @dst's own references.
 * Used by fork/clone, where the strings are copied field by field next to
 * this call. */
void vfs_task_fs_pins_copy(task_t *dst, const task_t *src) {
    if (!dst || !src)
        return;
    dst->fs.cwd_vn = NULL;
    dst->fs.root_vn = NULL;
    dst->fs.root_mnt = NULL;
    if (src->fs.cwd_vn) {
        dst->fs.cwd_vn = src->fs.cwd_vn;
        vnode_get(dst->fs.cwd_vn);
        if (dst->fs.cwd_vn->mnt)
            vfs_mount_cwd_get(dst->fs.cwd_vn->mnt);
    }
    dst->fs.root_vn = src->fs.root_vn;
    if (dst->fs.root_vn)
        vnode_get(dst->fs.root_vn);
    dst->fs.root_mnt = src->fs.root_mnt;
    if (dst->fs.root_mnt)
        vfs_mount_root_get(dst->fs.root_mnt);
}

/* Drop the per-task references.  fdtable_close_all() calls this on every
 * exit path; it must be safe to call twice (exec re-enters teardown). */
void vfs_task_fs_pins_release(task_t *t) {
    if (!t) return;
    if (t->fs.cwd_vn) {
        if (t->fs.cwd_vn->mnt)
            vfs_mount_cwd_put(t->fs.cwd_vn->mnt);
        vnode_put(t->fs.cwd_vn);
        t->fs.cwd_vn = NULL;
    }
    if (t->fs.root_vn) {
        vnode_put(t->fs.root_vn);
        t->fs.root_vn = NULL;
    }
    if (t->fs.root_mnt) {
        vfs_mount_root_put(t->fs.root_mnt);
        t->fs.root_mnt = NULL;
    }
}

/* The mount @t is rooted in, resolving the implicit "/" root on first use
 * without taking a reference (nothing can drop the namespace root while any
 * process exists). */
mount_t *vfs_task_root_mount(task_t *t) {
    if (!t) return NULL;
    if (t->fs.root_mnt) return t->fs.root_mnt;
    return vfs_find_mount("/");
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

    const char *root = cur->fs.root_path[0] ? cur->fs.root_path : "/";
    if (strcmp(root, "/") != 0 && !path_is_beneath(root, canon))
        return -EACCES;

    vnode_t *vn = vfs_resolve(canon);
    if (!vn) return vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
    if (vn->type != VFS_FT_DIR) { vnode_put(vn); return -ENOTDIR; }
    if (vfs_vnode_permission(vn, X_OK) < 0) { vnode_put(vn); return -EACCES; }
    char visible[MAX_PATH_LEN];
    const char *cwd_visible =
        vfs_chroot_visible_path(cur, canon, visible, sizeof(visible));
    if (!cwd_visible) { vnode_put(vn); return -ENAMETOOLONG; }
    /* The reference @vn arrived with is adopted as the cwd pin rather than
     * dropped: it is what makes "somebody is standing in this filesystem"
     * answerable at umount time. */
    vfs_task_cwd_set(cur, vn, cwd_visible);
    vnode_put(vn);
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

/*
 * Linux file_can_poll() equivalent.  A file may join an epoll set only when
 * its f_op provides ->poll.  Regular, procfs, sysfs and cgroup files have no
 * real ->poll: poll() still reports them always ready (as Linux does), but
 * epoll_ctl() must reject them with EPERM, or an always-ready fd makes
 * epoll_wait() return immediately forever and the event loop spins at 100%.
 * devfs device nodes keep the historical fallback because some of them (fb,
 * kmsg, ...) reach readiness only through the vnode path.
 */
int vfs_file_is_pollable(vfile_t *vf)
{
    if (!vf || !vf->ops)
        return 0;
    if (vf->ops->poll)
        return 1;
    if (signalfd_poll_events(vf, 0) >= 0)
        return 1;
    if (vfs_is_pipe_vfile(vf))
        return 1;
    if (vfs_is_char_device_vfile(vf))
        return 1;
    if (vf->vnode && vf->vnode->mnt && vf->vnode->mnt->type == FS_TYPE_DEVFS)
        return 1;
    return 0;
}

int vfs_poll_file(vfile_t *vf, short events) {
    if (!vf) return POLLNVAL;
    if (tty_control_revoked(vf)) return POLLHUP | POLLERR | (events & (POLLIN | POLLOUT));
    int sr = signalfd_poll_events(vf, events);
    if (sr >= 0)
        return sr;

    if (vf->ops && vf->ops->poll)
        return vf->ops->poll(vf, events);

    short revents = 0;
    if (vfs_is_pipe_vfile(vf))
        return pipe_poll_events(vf, events);

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
        return revents;
    }

    if (vf->vnode) {
        if (events & POLLIN)
            revents |= POLLIN;
        if (events & POLLOUT)
            revents |= POLLOUT;
        return revents;
    }

    return POLLNVAL;
}

int vfs_poll_events(int fd, short events) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return POLLNVAL;
    int revents = vfs_poll_file(vf, events);
    vfs_put_file_ref(fd, vf);
    return revents;
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

void vfs_release_process_file_locks(vfile_t *vf, int pid) {
    if (!vf) return;
    fs_locks_release_process_file(vf, pid);
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
    int r;
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        r = fdtable_dup_current(fd, (int)arg,
                                cmd == F_DUPFD_CLOEXEC ? O_CLOEXEC : 0);
        vfs_put_file(vf);
        return r;
    }
    r = vfs_fcntl_vfile(vf, cmd, arg);
    vfs_put_file(vf);
    return r;
}

/* fcntl on an already-resolved vfile (native ABI file handles).  The caller
 * owns @vf's reference.  F_DUPFD/F_DUPFD_CLOEXEC need an fd to duplicate and
 * are only reachable through the fd-level vfs_fcntl(). */
int vfs_fcntl_vfile(vfile_t *vf, int cmd, long arg) {
    if (!vf) return -EBADF;
    task_t *t = proc_current();
    int owner = t ? t->pid : 0;
#define VFS_FCNTL_RETURN(expr) do { int _vfs_fcntl_r = (expr); return _vfs_fcntl_r; } while (0)

    if (cmd == F_GETFL)
        VFS_FCNTL_RETURN(vf->flags);
    if (cmd == F_SETFL) {
        int accmode = vf->flags & O_ACCMODE;
        vf->flags = accmode | ((int)arg & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
        VFS_FCNTL_RETURN(0);
    }
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
        VFS_FCNTL_RETURN(-EINVAL);
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
    quota_init();

    {
        mount_t *mnt = vfs_mount_alloc();
        strcpy(mnt->path, "/");
        mnt->type = FS_TYPE_RAMFS;
        strcpy(mnt->dev, "none");
        strcpy(mnt->fstype, "rootfs");
        strcpy(mnt->opts, "rw");
        mnt->root = ramfs_mount(mnt);
        vfs_mount_link_tree(mnt);
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
        vfs_mount_link_tree(mnt);
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
        vfs_mount_link_tree(shm_mnt);
    }

    /* std streams: no global table any more.  Boot-context fd operations
     * lazily wire stdio into the pinned boot table on first use (fs/fdtable.c
     * fdtable_boot()), and every task gets its own via fdtable_init_stdio(). */

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
            vfs_mount_link_tree(mnt);
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
            vfs_mount_link_tree(mnt);
            kdebug("[VFS] Mounted sysfs at /sys\n");
        }
    }
}
