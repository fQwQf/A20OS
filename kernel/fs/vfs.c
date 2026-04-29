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
#include "drv/virtio_blk.h"
#include "fs/block_cache.h"
#include "net/socket.h"


static vnode_t *vfs_resolve_no_follow_final(const char *path);

static void vfs_release_open_file_locks(vfile_t *vf, int gfd);

static int g_lookup_errno;

/* ============================================================
 * VFS path resolution → vnode
 * ============================================================ */



/* Resolve an absolute path within a vnode tree */
static vnode_t *vnode_lookup_path(vnode_t *root, const char *path) {
    if (!root) return NULL;
    g_lookup_errno = 0;

    vnode_t *cur = root;
    vnode_get(cur);

    if (!path || !*path) return cur;

    char buf[MAX_PATH_LEN];
    strncpy(buf, path, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';

    char *p = buf;
    while (*p == '/') p++;

    int symlink_depth = 0;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (*p == '\0') {
        } else if (strcmp(p, ".") == 0) {
            /* stay */
        } else if (strcmp(p, "..") == 0) {
            if (cur->parent && cur->parent != cur) {
                vnode_t *parent = cur->parent;
                vnode_get(parent);
                vnode_put(cur);
                cur = parent;
            }
        } else {
            if (strlen(p) >= MAX_NAME_LEN) {
                vnode_put(cur);
                g_lookup_errno = -ENAMETOOLONG;
                return NULL;
            }
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                vnode_put(cur);
                g_lookup_errno = -ENOTDIR;
                return NULL;
            }
            if (vfs_vnode_permission(cur, X_OK) < 0) {
                vnode_put(cur);
                g_lookup_errno = -EACCES;
                return NULL;
            }
            vnode_t *next = vfs_dcache_lookup(cur, p);
            if (!next) {
                int r = cur->ops->lookup(cur, p, &next);
                if (r < 0 || !next) {
                    vnode_put(cur);
                    g_lookup_errno = r < 0 ? r : -ENOENT;
                    return NULL;
                }
                vfs_dcache_insert(cur, p, next);
            }
            vnode_t *parent = cur;
            cur = next;

            if (cur->type == VFS_FT_SYMLINK) {
                if (++symlink_depth > 8) {
                    vnode_put(parent);
                    vnode_put(cur);
                    g_lookup_errno = -ELOOP;
                    return NULL;
                }
                if (!cur->ops || !cur->ops->readlink) {
                    vnode_put(parent);
                    vnode_put(cur);
                    return NULL;
                }
                char link_target[MAX_PATH_LEN];
                int len = cur->ops->readlink(cur, link_target, sizeof(link_target));
                if (len < 0) {
                    vnode_put(parent);
                    vnode_put(cur);
                    return NULL;
                }
                link_target[len] = '\0';

                char rest[MAX_PATH_LEN];
                if (sep) {
                    snprintf(rest, sizeof(rest), "%s/%s", link_target, sep + 1);
                } else {
                    strncpy(rest, link_target, sizeof(rest) - 1);
                    rest[sizeof(rest) - 1] = '\0';
                }

                vnode_t *old = cur;
                if (link_target[0] == '/') {
                    cur = root;
                    vnode_get(cur);
                } else {
                    cur = parent;
                    vnode_get(cur);   /* compensate: we reuse parent, but it gets decremented below */
                }
                vnode_put(old);
                vnode_put(parent);

                strncpy(buf, rest, MAX_PATH_LEN - 1);
                buf[MAX_PATH_LEN - 1] = '\0';
                p = buf;
                while (*p == '/') p++;
                continue;
            }
            vnode_put(parent);
        }

        if (sep) p = sep + 1;
        else break;
    }
    return cur;
}

vnode_t *vfs_resolve(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    return vfs_resolve_at(path, cwd);
}

vnode_t *vfs_resolve_at(const char *path, const char *cwd) {
    char resolved[MAX_PATH_LEN];

    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;
    if (vfs_path_normalize_absolute(resolved) < 0)
        return NULL;

    /* Find best matching mount */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) return NULL;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    return vnode_lookup_path(mnt->root, rel);
}

/* ============================================================
 * VFS open / close
 * ============================================================ */

extern vfile_t *fat32_open_vnode(vnode_t *vn, int flags);
extern vfile_t *ext4_open_vnode(vnode_t *vn, int flags);

int vfs_open(const char *path, int flags, int mode) {
    /* Resolve cwd from current process */
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    /* Check for special device files */
    char resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    if (strcmp(resolved, "/proc/self/exe") == 0) {
        task_t *cur = proc_current();
        const char *exe = cur && cur->exec_path[0] ? cur->exec_path : "/bin/sh";
        return vfs_open(exe, flags, mode);
    }

    /* Find mount point */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) { kdebug("[VFS] open '%s': no mount\n", resolved); return -ENOENT; }

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    vnode_t *vn = vnode_lookup_path(mnt->root, rel);

    if (!vn) {
        if (g_lookup_errno && !(flags & O_CREAT)) return g_lookup_errno;
        if (!(flags & O_CREAT)) { kdebug("[VFS] open '%s' (rel='%s'): not found, no O_CREAT\n", resolved, rel); return -ENOENT; }
        if (!mnt->root || !mnt->root->ops || !mnt->root->ops->create) { kdebug("[VFS] open '%s': root has no create ops\n", resolved); return -ENOSYS; }

        char parent_path[MAX_PATH_LEN];
        char fname[MAX_NAME_LEN];
        int sr = vfs_path_split_parent_name(rel, parent_path, sizeof(parent_path),
                                            fname, sizeof(fname));
        if (sr < 0)
            return sr;

        vnode_t *parent = vnode_lookup_path(mnt->root, parent_path);
        if (!parent || parent->type != VFS_FT_DIR) {
            kdebug("[VFS] open '%s': parent '%s' not found\n", resolved, parent_path);
            vnode_put(parent);
            return g_lookup_errno ? g_lookup_errno : -ENOENT;
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

        int cmode = (mode & 07777) & ~(cur ? cur->fs.umask : 022);
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
            vfs_dcache_invalidate_all();
        }
    }

    vfile_t *vf = NULL;
    if (mnt->type == FS_TYPE_FAT32) {
        vf = fat32_open_vnode(vn, flags);
    } else if (mnt->type == FS_TYPE_EXT4) {
        vf = ext4_open_vnode(vn, flags);
    } else if (mnt->type == FS_TYPE_RAMFS) {
        vf = ramfs_open_vnode(vn, flags);
    } else if (mnt->type == FS_TYPE_PROCFS) {
        extern vfile_t *procfs_open_vnode(vnode_t *vn, int flags);
        vf = procfs_open_vnode(vn, flags);
    } else if (mnt->type == FS_TYPE_DEVFS) {
        vf = devfs_open_vnode(vn, flags);
    }

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
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        vfile_free(vf);
        vnode_put(vn);
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
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return g_lookup_errno ? g_lookup_errno : -ENOENT;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        vnode_put(parent);
        return -ENOTDIR;
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
        vnode_put(victim);
        if (sr < 0) {
            vnode_put(parent);
            return sr;
        }
    }

    int r = parent->ops->unlink(parent, name);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_rename(const char *old, const char *newpath) {
    if (!old || !newpath) return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    char old_resolved[MAX_PATH_LEN];
    char new_resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, old, old_resolved, sizeof(old_resolved));
    if (pr < 0)
        return pr;
    pr = vfs_path_join(cwd, newpath, new_resolved, sizeof(new_resolved));
    if (pr < 0)
        return pr;

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
        int sr = vfs_sticky_may_remove(new_dir, new_victim);
        vnode_put(new_victim);
        if (sr < 0) {
            vnode_put(old_dir);
            vnode_put(new_dir);
            return sr;
        }
    }

    int r = old_dir->ops->rename(old_dir, old_name, new_dir, new_name);
    vnode_put(old_dir);
    vnode_put(new_dir);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_rmdir(const char *path) {
    if (!path) return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    char resolved[MAX_PATH_LEN];
    int pr = vfs_path_join(cwd, path, resolved, sizeof(resolved));
    if (pr < 0)
        return pr;

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    int sr = vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                        name, sizeof(name));
    if (sr < 0)
        return sr;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt || !mnt->root) return -ENOENT;

    vnode_t *parent = vnode_lookup_path(mnt->root, vfs_strip_mount_prefix(parent_path, mnt));
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return g_lookup_errno ? g_lookup_errno : -ENOENT;
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
        vnode_put(victim);
        if (sr < 0) {
            vnode_put(parent);
            return sr;
        }
    }

    int r = parent->ops->rmdir(parent, name);
    vnode_put(parent);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

static vnode_t *vfs_resolve_no_follow_final(const char *path) {
    if (!path || !*path) return NULL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->fs.cwd : "/";

    char resolved[MAX_PATH_LEN];
    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;
    vfs_path_trim_trailing_slashes(resolved);

    if (strcmp(resolved, "/") == 0)
        return vfs_resolve(resolved);

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    if (vfs_path_split_parent_name(resolved, parent_path, sizeof(parent_path),
                                   name, sizeof(name)) < 0)
        return NULL;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return NULL;
    const char *rel = vfs_strip_mount_prefix(parent_path, mnt);
    vnode_t *parent = vnode_lookup_path(mnt->root, rel);
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

int vfs_stat(const char *path, kstat_t *st) {
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
        r = vfs_mode_has_perm_ids(st.st_mode, st.st_uid, st.st_gid, uid, gid, mode);
    }
    vnode_put(vn);
    return r;
}

static int vfs_chmod_vnode(vnode_t *vn, int mode) {
    if (!vn) return -ENOENT;
    if (!vfs_current_owns(vn)) return -EPERM;
    mode &= 07777;
    if (!proc_has_cap(proc_current(), CAP_FOWNER)) {
        kstat_t st;
        if (vfs_vnode_stat(vn, &st) == 0 && !vfs_task_in_group(proc_current(), st.st_gid))
            mode &= ~S_ISGID;
    }
    if (vn->ops && vn->ops->chmod) {
        int r = vn->ops->chmod(vn, mode);
        if (r == 0)
            vfs_dcache_invalidate_all();
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
    if (uid < -1 || gid < -1) return -EINVAL;
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
        if (r == 0)
            vfs_dcache_invalidate_all();
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
        size_t len = strlen(exe);
        if (len >= sz) len = sz - 1;
        memcpy(buf, exe, len);
        buf[len] = '\0';
        return (int)len;
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
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOENT;
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
        if ((events & POLLIN) && (fd != 0 || uart_has_input()))
            revents |= POLLIN;
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

static int vfs_fcntl_getlk(vfile_t *vf, long arg, int owner_kind, int owner) {
    if (!arg) return -EFAULT;
    fs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    int r = fs_locks_get(vf, &lk, owner_kind, owner);
    if (r < 0) return r;
    return copy_to_user((void *)arg, &lk, sizeof(lk)) < 0 ? -EFAULT : 0;
}

static int vfs_fcntl_setlk(vfile_t *vf, long arg, int owner_kind, int owner, int wait) {
    if (!arg) return -EFAULT;
    fs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    return fs_locks_set(vf, &lk, owner_kind, owner, wait);
}

void vfs_release_process_locks(int pid) {
    fs_locks_release_process(pid);
}

static void vfs_release_open_file_locks(vfile_t *vf, int gfd) {
    fs_locks_release_file(vf, gfd);
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
        VFS_FCNTL_RETURN(vfs_fcntl_getlk(vf, arg, FS_LOCK_OWNER_OFD, fd));
    if (cmd == F_OFD_SETLK)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_OFD, fd, 0));
    if (cmd == F_OFD_SETLKW)
        VFS_FCNTL_RETURN(vfs_fcntl_setlk(vf, arg, FS_LOCK_OWNER_OFD, fd, 1));

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

    if (cmd == F_SETLEASE || cmd == F_NOTIFY || cmd == F_CANCELLK)
        VFS_FCNTL_RETURN(0);
    if (cmd == F_GETLEASE)
        VFS_FCNTL_RETURN(F_UNLCK);

    VFS_FCNTL_RETURN(-EINVAL);
#undef VFS_FCNTL_RETURN
}

/* ============================================================
 * VFS Mount
 * ============================================================ */

int vfs_mount(const char *dev, const char *path, const char *fstype, int flags) {
    (void)dev;
    if (!path || !fstype) return -EINVAL;
    if (strcmp(fstype, "cgroup") == 0 || strcmp(fstype, "cgroup2") == 0)
        return -ENODEV;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *existing = vfs_mount_at(i);
        if (existing && strcmp(existing->path, path) == 0) {
            existing->flags = flags;
            vfs_dcache_invalidate_all();
            return 0;
        }
    }
    if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0) {
        vnode_t *target = vfs_resolve(path);
        if (!target) return -ENOENT;
        int is_dir = target->type == VFS_FT_DIR;
        vnode_put(target);
        if (!is_dir) return -ENOTDIR;

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type = FS_TYPE_RAMFS;
        mnt->flags = flags;
        mnt->root = ramfs_mount_empty(mnt);
        if (!mnt->root) {
            vfs_mount_remove(mnt);
            return -ENOMEM;
        }
        mnt->root->mnt = mnt;
        vfs_dcache_invalidate_all();
        return 0;
    }
    printf("[VFS] vfs_mount: use vfs_mount_bc() for block filesystems\n");
    return -EINVAL;
}

int vfs_mount_bc(const char *path, const char *fstype, bcache_t *bc) {
    if (strcmp(fstype, "fat32") == 0 || strcmp(fstype, "vfat") == 0) {
        if (!bc) { printf("[VFS] No bcache for FAT32 mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = fat32_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_FAT32;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;

        printf("[VFS] Mounted FAT32 at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    if (strcmp(fstype, "ext4") == 0) {
        if (!bc) { printf("[VFS] No bcache for ext4 mount\n"); return -ENODEV; }

        mount_t *mnt = vfs_mount_alloc();
        if (!mnt) return -ENOMEM;
        vnode_t *root = ext4_mount(bc);
        if (!root) {
            vfs_mount_remove(mnt);
            return -EIO;
        }

        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_EXT4;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;

        printf("[VFS] Mounted ext4 at %s\n", path);
        vfs_dcache_invalidate_all();
        return 0;
    }

    printf("[VFS] Unknown fstype: %s\n", fstype);
    return -EINVAL;
}

int vfs_umount(const char *path) {
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (mnt && strcmp(mnt->path, path) == 0) {
            vfs_dcache_invalidate_all();
            if (mnt->type == FS_TYPE_FAT32) {
                fat32_unmount(mnt->root);
            } else if (mnt->type == FS_TYPE_EXT4) {
                ext4_unmount(mnt->root);
            }
            vfs_mount_remove(mnt);
            return 0;
        }
    }
    return -EINVAL;
}

/* Truncate */
int vfs_truncate(const char *path, size_t size) {
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return -ENOENT;
    int r = -ENOSYS;
    if (vn->ops && vn->ops->truncate) r = vn->ops->truncate(vn, size);
    if (r == 0)
        page_cache_truncate(vn, size);
    vnode_put(vn);
    if (r == 0)
        vfs_dcache_invalidate_all();
    return r;
}

int vfs_ftruncate(int fd, size_t size) {
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf) return -EBADF;
    int r = 0;
    if (!vfs_should_write(vf->flags)) r = -EINVAL;
    else if (!vf->vnode || !vf->vnode->ops || !vf->vnode->ops->truncate) r = -EINVAL;
    else if ((vf->seals & F_SEAL_SHRINK) && size < vf->vnode->size) r = -EPERM;
    else if ((vf->seals & F_SEAL_GROW) && size > vf->vnode->size) r = -EPERM;
    else if (vf->seals & F_SEAL_WRITE) r = -EPERM;
    else r = vf->vnode->ops->truncate(vf->vnode, size);
    if (r == 0) {
        page_cache_truncate(vf->vnode, size);
        vfs_dcache_invalidate_all();
    }
    vfs_put_file_ref(fd, vf);
    return r;
}

/* ============================================================
 * VFS init — set up std streams, root ramfs mount
 * ============================================================ */

void vfs_init(void) {
    file_table_init();
    vfs_mount_table_init();
    if (page_cache_init() < 0)
        printf("[VFS] page cache init failed; continuing without it\n");

    /* Register ramfs as root "/" mount */
    mount_t *mnt = vfs_mount_alloc();
    strcpy(mnt->path, "/");
    mnt->type = FS_TYPE_RAMFS;
    mnt->root = ramfs_mount(mnt);

    printf("[VFS] Initialized (root=ramfs)\n");

    vfs_mkdir("/dev", 0755);
    mnt = vfs_mount_alloc();
    strcpy(mnt->path, "/dev");
    mnt->type = FS_TYPE_DEVFS;
    mnt->root = devfs_mount();
    if (mnt->root) mnt->root->mnt = mnt;

    /* Install std streams at global fds 0,1,2 */
    file_install_at(STDIN_FILENO, devfs_create_stdio(STDIN_FILENO));
    file_install_at(STDOUT_FILENO, devfs_create_stdio(STDOUT_FILENO));
    file_install_at(STDERR_FILENO, devfs_create_stdio(STDERR_FILENO));

    vfs_mkdir("/tmp", 0755);
    vfs_mkdir("/etc", 0755);
    {
        static const char passwd[] =
            "root:x:0:0:root:/root:/bin/sh\n"
            "nobody:x:65534:65534:nobody:/nonexistent:/bin/false\n";
        static const char group[] =
            "root:x:0:\n"
            "nobody:x:65534:\n";
        int fd = vfs_open("/etc/passwd", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            vfs_write(fd, passwd, sizeof(passwd) - 1);
            vfs_close(fd);
        }
        fd = vfs_open("/etc/group", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0) {
            vfs_write(fd, group, sizeof(group) - 1);
            vfs_close(fd);
        }
    }

    /* Mount procfs at /proc */
    {
        extern vnode_t *procfs_mount(void);
        extern vfile_t *procfs_open_vnode(vnode_t *vn, int flags);
        vfs_mkdir("/proc", 0755);
        vnode_t *procfs_root = procfs_mount();
        if (procfs_root) {
            mount_t *mnt = vfs_mount_alloc();
            strcpy(mnt->path, "/proc");
            mnt->type = FS_TYPE_PROCFS;
            mnt->root = procfs_root;
            procfs_root->mnt = mnt;
            printf("[VFS] Mounted procfs at /proc\n");
        }
    }
}
