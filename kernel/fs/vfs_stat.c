#include "fs/vfs.h"
#include "fs/vfs/file.h"
#include "fs/vfs/path.h"
#include "fs/vfs/stat_perm.h"
#include "fs/vfs/mount.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/page_cache.h"
#include "fs/ext4.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"

/*
 * Stat / permission / timestamp operations, split out of fs/vfs.c.
 * Pure metadata paths: resolve a vnode, then query or update mode, ownership,
 * size and timestamps.  Path resolution and fd lifecycle stay in fs/vfs.c.
 */

static int vfs_vfile_stat(vfile_t *vf, kstat_t *st)
{
    if (vf->vnode)
        return vfs_vnode_stat(vf->vnode, st);
    if (vfs_is_char_device_vfile(vf)) {
        fill_char_kstat(st);
        return 0;
    }
    if (vfs_is_pipe_vfile(vf)) {
        fill_pipe_kstat(st);
        return 0;
    }
    return -EBADF;
}

static int vfs_proc_fd_stat(const char *path, kstat_t *st, int *matched)
{
    task_t *task = NULL;
    int fd = -1;
    int match = vfs_proc_fd_target(path, &task, &fd);
    *matched = match != 0;
    if (match <= 0)
        return match;
    if (!proc_task_may_access(proc_current(), task)) {
        proc_put(task);
        return -EACCES;
    }
    int gfd = -1;
    vfile_t *vf = fdtable_get_file_ref(task, fd, &gfd, NULL);
    proc_put(task);
    if (!vf)
        return -ENOENT;
    int r = vfs_vfile_stat(vf, st);
    vfs_put_file_ref(gfd, vf);
    return r;
}

int vfs_statx(const char *path, kstat_t *st, unsigned int mask, int sync_hint) {
    int proc_fd_match = 0;
    int proc_fd_result = vfs_proc_fd_stat(path, st, &proc_fd_match);
    if (proc_fd_match)
        return proc_fd_result;
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
    int proc_fd_match = 0;
    int proc_fd_result = vfs_proc_fd_stat(path, st, &proc_fd_match);
    if (proc_fd_match)
        return proc_fd_result;
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    int r = vfs_vnode_stat(vn, st);
    vnode_put(vn);
    return r;
}

int vfs_fstat(int fd, kstat_t *st) {
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = vf ? vfs_vfile_stat(vf, st) : -EBADF;
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_statfs(vnode_t *vn, kstatfs_t *st) {
    if (!vn || !st)
        return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->f_bsize = PAGE_SIZE;
    st->f_frsize = PAGE_SIZE;
    st->f_namelen = MAX_NAME_LEN;
    if (vn->mnt) {
        st->f_flags = (uint64_t)(unsigned int)vn->mnt->flags;
        switch (vn->mnt->type) {
        case FS_TYPE_FAT32:  st->f_type = 0x4d44; break;
        case FS_TYPE_EXT4:   st->f_type = EXT4_DISK_MAGIC; break;
        case FS_TYPE_ISOFS:  st->f_type = 0x9660; break;
        case FS_TYPE_PROCFS: st->f_type = 0x9fa0; break;
        case FS_TYPE_DEVFS:  st->f_type = 0x01021994; break;
        case FS_TYPE_CGROUP: st->f_type = 0x63677270; break;
        case FS_TYPE_SYSFS:  st->f_type = 0x62656572; break;
        case FS_TYPE_RAMFS:
        default:             st->f_type = 0x858458f6; break;
        }
    } else {
        st->f_type = 0x858458f6;
    }

    /*
     * Synthetic filesystems deliberately report zero capacity.  Backed
     * filesystems override this through their vnode operation.
     */
    if (vn->ops && vn->ops->statfs)
        return vn->ops->statfs(vn, st);
    return 0;
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
