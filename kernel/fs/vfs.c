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
#include "core/timekeeping.h"
#include "drv/virtio_blk.h"
#include "fs/block_cache.h"
#include "net/socket.h"

/* ============================================================
 * Global open-file table
 * ============================================================ */
#define GFILE_MAX   VFS_MAX_OPEN
#define PIPE_DEFAULT_SIZE (16 * PIPE_BUF_SIZE)

static vfile_t *g_files[GFILE_MAX];  // 全局文件表
static spinlock_t g_file_lock = SPINLOCK_INIT;

/* Mount table (simple linear) */
#define MAX_MOUNTS  64
static mount_t g_mounts[MAX_MOUNTS];  // 挂载表
static int     g_nmounts = 0;  // 已挂载数量

static vnode_t *vfs_resolve_no_follow_final(const char *path);

/* ---- File descriptor allocation ---- */

// 分配全局文件描述符
int vfs_alloc_fd(vfile_t *vf) {
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    /* Find slot in global file table */
    int gfd = -1;
    for (int i = 3; i < GFILE_MAX; i++) { /* 0,1,2 reserved for std??? */
        if (!g_files[i]) { g_files[i] = vf; gfd = i; break; }
    }
    spin_unlock_irqrestore(&g_file_lock, flags);
    return gfd;
}

// 获取文件描述符对应的 vfile
vfile_t *vfs_get_file(int fd) {
    if (fd < 0 || fd >= GFILE_MAX) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    spin_unlock_irqrestore(&g_file_lock, flags);
    return vf;
}

/* ============================================================
 * Per-process fd table
 * We map per-process fds → global gfds
 * For simplicity, the per-process fd IS the global gfd.
 * ============================================================ */

// 初始化进程的文件描述符表
void vfs_proc_init_fds(int *fd_table) {
    for (int i = 0; i < MAX_FILES; i++) fd_table[i] = -1;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    fd_table[0] = 0; if (g_files[0]) g_files[0]->ref_count++;
    fd_table[1] = 1; if (g_files[1]) g_files[1]->ref_count++;
    fd_table[2] = 2; if (g_files[2]) g_files[2]->ref_count++;
    spin_unlock_irqrestore(&g_file_lock, flags);
}

// 初始化进程的标准 I/O 文件描述符（如果未设置）
void vfs_proc_init_stdio_defaults(int *fd_table) {
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    if (fd_table[0] < 0) { fd_table[0] = 0; if (g_files[0]) g_files[0]->ref_count++; }
    if (fd_table[1] < 0) { fd_table[1] = 1; if (g_files[1]) g_files[1]->ref_count++; }
    if (fd_table[2] < 0) { fd_table[2] = 2; if (g_files[2]) g_files[2]->ref_count++; }
    spin_unlock_irqrestore(&g_file_lock, flags);
}

// 复制进程文件描述符表（用于 fork）
void vfs_proc_copy_fds(const int *src, int *dst) {
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    for (int i = 0; i < MAX_FILES; i++) {
        dst[i] = src[i];
        if (src[i] >= 0 && src[i] < GFILE_MAX && g_files[src[i]]) {
            g_files[src[i]]->ref_count++;
        }
    }
    spin_unlock_irqrestore(&g_file_lock, flags);
}

void vfs_proc_close_all_fds(int *fd_table) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (fd_table[i] >= 0) {
            vfs_close(fd_table[i]);
        }
        fd_table[i] = -1;
    }
}

static vfile_ops_t g_pipe_read_ops;
static vfile_ops_t g_pipe_write_ops;

#define VFS_TIME_META_MAX 8192
typedef struct {
    int used;
    void *mnt;
    uint64_t ino;
    uint64_t atime;
    uint64_t atime_nsec;
    uint64_t mtime;
    uint64_t mtime_nsec;
    uint64_t ctime;
    uint64_t ctime_nsec;
} vfs_time_meta_t;

static vfs_time_meta_t g_time_meta[VFS_TIME_META_MAX];
static void vfs_touch_mtime(vnode_t *vn);
static void vfs_release_open_file_locks(vfile_t *vf, int gfd);

static int is_pipe_vfile(vfile_t *vf) {
    return vf && (vf->ops == &g_pipe_read_ops || vf->ops == &g_pipe_write_ops);
}

static int is_char_device_vfile(vfile_t *vf) {
    return devfs_is_char_vfile(vf);
}

static void fill_char_kstat(kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

static void fill_pipe_kstat(kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    task_t *cur = proc_current();
    st->st_uid = cur ? (uint32_t)cur->uid : 0;
    st->st_gid = cur ? (uint32_t)cur->gid : 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

static int vfs_task_is_root(task_t *t) {
    return !t || t->euid == 0;
}

static int vfs_task_in_group(task_t *t, uint32_t gid) {
    if (!t) return gid == 0;
    if ((uint32_t)t->fsgid == gid || (uint32_t)t->egid == gid || (uint32_t)t->gid == gid)
        return 1;
    for (int i = 0; i < t->ngroups && i < MAX_GROUPS; i++) {
        if ((uint32_t)t->groups[i] == gid)
            return 1;
    }
    return 0;
}

static int vfs_ids_in_group(task_t *t, uint32_t primary_gid, uint32_t gid) {
    if (primary_gid == gid) return 1;
    if (!t) return gid == 0;
    for (int i = 0; i < t->ngroups && i < MAX_GROUPS; i++) {
        if ((uint32_t)t->groups[i] == gid)
            return 1;
    }
    return 0;
}

static int vfs_mode_has_perm_ids(uint32_t st_mode, uint32_t file_uid, uint32_t file_gid,
                                 uint32_t uid, uint32_t gid, int mask) {
    task_t *cur = proc_current();
    if (mask == F_OK) return 0;

    if (uid == 0) {
        if ((mask & X_OK) && ((st_mode & S_IFMT) == S_IFREG) &&
            !(st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return -EACCES;
        return 0;
    }

    int shift = 0;
    if (uid == file_uid)
        shift = 6;
    else if (vfs_ids_in_group(cur, gid, file_gid))
        shift = 3;

    uint32_t need = 0;
    if (mask & R_OK) need |= 4;
    if (mask & W_OK) need |= 2;
    if (mask & X_OK) need |= 1;
    return (((st_mode >> shift) & need) == need) ? 0 : -EACCES;
}

static int vfs_mode_has_perm(uint32_t st_mode, uint32_t uid, uint32_t gid, int mask) {
    task_t *cur = proc_current();
    return vfs_mode_has_perm_ids(st_mode, uid, gid,
                                 cur ? (uint32_t)cur->fsuid : 0,
                                 cur ? (uint32_t)cur->fsgid : 0,
                                 mask);
}

static int vfs_vnode_stat(vnode_t *vn, kstat_t *st) {
    if (!vn || !st) return -EINVAL;
    int r = 0;
    if (vn->ops && vn->ops->stat) {
        r = vn->ops->stat(vn, st);
        if (r < 0) return r;
    } else {
        memset(st, 0, sizeof(*st));
        st->st_ino = vn->ino;
        st->st_mode = vn->mode;
        st->st_uid = vn->uid;
        st->st_gid = vn->gid;
        st->st_size = vn->size;
        st->st_nlink = 1;
    }
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (g_time_meta[i].used && g_time_meta[i].mnt == vn->mnt &&
            g_time_meta[i].ino == vn->ino) {
            st->st_atime = g_time_meta[i].atime;
            st->st_atime_nsec = g_time_meta[i].atime_nsec;
            st->st_mtime = g_time_meta[i].mtime;
            st->st_mtime_nsec = g_time_meta[i].mtime_nsec;
            st->st_ctime = g_time_meta[i].ctime;
            st->st_ctime_nsec = g_time_meta[i].ctime_nsec;
            break;
        }
    }
    if (st->st_atime == 0 && st->st_mtime == 0 && st->st_ctime == 0) {
        uint64_t now[2];
        timekeeping_get_realtime(now);
        st->st_atime = now[0];
        st->st_atime_nsec = now[1];
        st->st_mtime = now[0];
        st->st_mtime_nsec = now[1];
        st->st_ctime = now[0];
        st->st_ctime_nsec = now[1];
    }
    return 0;
}

static int vfs_vnode_permission(vnode_t *vn, int mask) {
    kstat_t st;
    int r = vfs_vnode_stat(vn, &st);
    if (r < 0) return r;
    return vfs_mode_has_perm(st.st_mode, st.st_uid, st.st_gid, mask);
}

static int vfs_current_owns(vnode_t *vn) {
    task_t *cur = proc_current();
    if (vfs_task_is_root(cur)) return 1;
    kstat_t st;
    if (vfs_vnode_stat(vn, &st) < 0) return 0;
    return cur && (uint32_t)cur->fsuid == st.st_uid;
}

static int vfs_should_read(int flags) {
    int acc = flags & O_ACCMODE;
    return acc == O_RDONLY || acc == O_RDWR;
}

static int vfs_should_write(int flags) {
    int acc = flags & O_ACCMODE;
    return acc == O_WRONLY || acc == O_RDWR;
}

static int g_lookup_errno;

/* ============================================================
 * Mount resolution — find which mount owns a path
 * ============================================================ */

static mount_t *vfs_find_mount(const char *path) {
    mount_t *best = NULL;
    size_t   best_len = 0;
    for (int i = 0; i < g_nmounts; i++) {
        size_t len = strlen(g_mounts[i].path);
        if (strncmp(path, g_mounts[i].path, len) == 0 &&
            (len == 1 || path[len] == '\0' || path[len] == '/') &&
            len > best_len) {
            best = &g_mounts[i];
            best_len = len;
        }
    }
    return best;
}

/* Strip the mount prefix from path */
static const char *strip_mount_prefix(const char *path, const mount_t *mnt) {
    size_t len = strlen(mnt->path);
    if (strncmp(path, mnt->path, len) == 0) {
        const char *rest = path + len;
        if (*rest == '/') rest++;
        return rest;
    }
    return path;
}

/* ============================================================
 * VFS path resolution → vnode
 * ============================================================ */



/* Resolve an absolute path within a vnode tree */
static vnode_t *vnode_lookup_path(vnode_t *root, const char *path) {
    if (!root) return NULL;
    g_lookup_errno = 0;

    vnode_t *cur = root;
    cur->ref_count++;

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
                parent->ref_count++;
                cur->ref_count--;
                cur = parent;
            }
        } else {
            if (strlen(p) >= MAX_NAME_LEN) {
                cur->ref_count--;
                g_lookup_errno = -ENAMETOOLONG;
                return NULL;
            }
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                cur->ref_count--;
                g_lookup_errno = -ENOTDIR;
                return NULL;
            }
            if (vfs_vnode_permission(cur, X_OK) < 0) {
                cur->ref_count--;
                g_lookup_errno = -EACCES;
                return NULL;
            }
            vnode_t *next = NULL;
            int r = cur->ops->lookup(cur, p, &next);
            if (r < 0 || !next) {
                cur->ref_count--;
                g_lookup_errno = r < 0 ? r : -ENOENT;
                return NULL;
            }
            vnode_t *parent = cur;
            cur = next;

            if (cur->type == VFS_FT_SYMLINK) {
                if (++symlink_depth > 8) {
                    parent->ref_count--;
                    cur->ref_count--;
                    g_lookup_errno = -ELOOP;
                    return NULL;
                }
                if (!cur->ops || !cur->ops->readlink) {
                    parent->ref_count--;
                    cur->ref_count--;
                    return NULL;
                }
                char link_target[MAX_PATH_LEN];
                int len = cur->ops->readlink(cur, link_target, sizeof(link_target));
                if (len < 0) {
                    parent->ref_count--;
                    cur->ref_count--;
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
                    cur->ref_count++;
                } else {
                    cur = parent;
                    cur->ref_count++;   /* compensate: we reuse parent, but it gets decremented below */
                }
                old->ref_count--;
                parent->ref_count--;

                strncpy(buf, rest, MAX_PATH_LEN - 1);
                buf[MAX_PATH_LEN - 1] = '\0';
                p = buf;
                while (*p == '/') p++;
                continue;
            }
            parent->ref_count--;
        }

        if (sep) p = sep + 1;
        else break;
    }
    return cur;
}

void vnode_put(vnode_t *vn) {
    if (!vn) return;
    if (vn->ref_count <= 0) {
        printf("[VFS BUG] vnode_put on freed vnode %p ino=%lu\n", (void *)vn, vn->ino);
        return;
    }
    vn->ref_count--;
    if (vn->ref_count <= 0) {
        if (vn->ops && vn->ops->release)
            vn->ops->release(vn);
    }
}

vnode_t *vfs_resolve(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
    return vfs_resolve_at(path, cwd);
}

vnode_t *vfs_resolve_at(const char *path, const char *cwd) {
    char resolved[MAX_PATH_LEN];

    if (path[0] == '/') {
        strncpy(resolved, path, MAX_PATH_LEN - 1);
    } else {
        snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    /* Normalize: resolve . and .. */
    char parts[64][MAX_NAME_LEN];
    int depth = 0;
    char *tok = resolved + 1; /* skip leading / */
    while (*tok) {
        char *end = strchr(tok, '/');
        size_t len = end ? (size_t)(end - tok) : strlen(tok);
        if (len == 0 || (len == 1 && tok[0] == '.')) {
            if (end) tok = end + 1; else break;
            continue;
        }
        if (len == 2 && tok[0] == '.' && tok[1] == '.') {
            if (depth > 0) depth--;
        } else if (depth < 64) {
            memcpy(parts[depth], tok, len < MAX_NAME_LEN ? len : MAX_NAME_LEN - 1);
            parts[depth][len < MAX_NAME_LEN ? len : MAX_NAME_LEN - 1] = '\0';
            depth++;
        }
        if (end) tok = end + 1; else break;
    }

    /* Reconstruct canonical path */
    char canon[MAX_PATH_LEN] = "/";
    for (int i = 0; i < depth; i++) {
        strcat(canon, parts[i]);
        if (i < depth - 1) strcat(canon, "/");
    }

    /* Find best matching mount */
    mount_t *mnt = vfs_find_mount(canon);
    if (!mnt) return NULL;

    const char *rel = strip_mount_prefix(canon, mnt);
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
    const char *cwd = cur ? cur->cwd : "/";

    /* Check for special device files */
    char resolved[MAX_PATH_LEN];
    if (path[0] == '/') strncpy(resolved, path, MAX_PATH_LEN - 1);
    else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(resolved, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    if (strcmp(resolved, "/proc/self/exe") == 0) {
        task_t *cur = proc_current();
        const char *exe = cur && cur->exec_path[0] ? cur->exec_path : "/bin/sh";
        return vfs_open(exe, flags, mode);
    }

    /* Find mount point */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) { kdebug("[VFS] open '%s': no mount\n", resolved); return -ENOENT; }

    const char *rel = strip_mount_prefix(resolved, mnt);
    vnode_t *vn = vnode_lookup_path(mnt->root, rel);

    if (!vn) {
        if (g_lookup_errno && !(flags & O_CREAT)) return g_lookup_errno;
        if (!(flags & O_CREAT)) { kdebug("[VFS] open '%s' (rel='%s'): not found, no O_CREAT\n", resolved, rel); return -ENOENT; }
        if (!mnt->root || !mnt->root->ops || !mnt->root->ops->create) { kdebug("[VFS] open '%s': root has no create ops\n", resolved); return -ENOSYS; }

        char parent_path[MAX_PATH_LEN];
        strncpy(parent_path, rel, MAX_PATH_LEN - 1);
        char *slash = strrchr(parent_path, '/');
        const char *fname = slash ? slash + 1 : rel;
        if (slash) *slash = '\0';
        else parent_path[0] = '\0';

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

        int cmode = (mode & 07777) & ~(cur ? cur->umask : 022);
        int r = parent->ops->create(parent, fname, cmode, &vn);
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
        if (mask && vfs_vnode_permission(vn, mask) < 0) {
            vnode_put(vn);
            return -EACCES;
        }
        if (vn->type == VFS_FT_DIR && vfs_should_write(flags)) {
            vnode_put(vn);
            return -EISDIR;
        }
    }

    if ((flags & O_TRUNC) && vn->type == VFS_FT_REGULAR && vn->ops && vn->ops->truncate)
        vn->ops->truncate(vn, 0);

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
        kfree(vf);
        return -EMFILE;
    }
    vnode_put(vn);
    return gfd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= GFILE_MAX) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    if (!vf) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBADF;
    }

    vf->ref_count--;
    if (vf->ref_count <= 0) {
        vnode_t *vn = vf->vnode;
        g_files[fd] = NULL;
        spin_unlock_irqrestore(&g_file_lock, flags);
        vfs_release_open_file_locks(vf, fd);
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        kfree(vf);
        vnode_put(vn);
        return 0;
    }
    spin_unlock_irqrestore(&g_file_lock, flags);
    return 0;
}

/* ============================================================
 * VFS read / write / lseek  
 * ============================================================ */

int vfs_read(int fd, char *buf, size_t count) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if ((vf->vnode || is_pipe_vfile(vf) || is_char_device_vfile(vf)) &&
            !vfs_should_read(vf->flags)) return -EBADF;
        if (vf->ops && vf->ops->read) return vf->ops->read(vf, buf, count);
        return -EBADF;
    }
    return -EBADF;
}

int vfs_write(int fd, const char *buf, size_t count) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if ((vf->vnode || is_pipe_vfile(vf) || is_char_device_vfile(vf)) &&
            !vfs_should_write(vf->flags)) return -EBADF;
        if (vf->seals & F_SEAL_WRITE) return -EPERM;
        if ((vf->seals & F_SEAL_GROW) && vf->vnode &&
            vf->offset + count > vf->vnode->size)
            return -EPERM;
        if (vf->ops && vf->ops->write) {
            int r = vf->ops->write(vf, buf, count);
            if (r > 0 && vf->vnode)
                vfs_touch_mtime(vf->vnode);
            return r;
        }
        return -EBADF;
    }
    return -EBADF;
}

long vfs_lseek(int fd, long offset, int whence) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (devfs_is_tty_vfile(vf) || is_pipe_vfile(vf)) return -ESPIPE;
        if (vf->ops && vf->ops->lseek) return vf->ops->lseek(vf, offset, whence);
    }
    return -EBADF;
}

int vfs_getdents64(int fd, void *dirp, size_t count) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->ops && vf->ops->readdir) return vf->ops->readdir(vf, dirp, count);
        return -EBADF;
    }
    return -EBADF;
}

int vfs_ioctl(int fd, unsigned long req, void *arg) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return -EBADF;
    if (vf->ops && vf->ops->ioctl) return vf->ops->ioctl(vf, req, arg);
    return -ENOTTY;
}

int vfs_sync(void) {
    for (int i = 0; i < g_nmounts; i++) {
        if (g_mounts[i].fs_data)
            bcache_sync((bcache_t *)g_mounts[i].fs_data);
    }
    return 0;
}

int vfs_fsync(int fd) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return -EBADF;
    if (vf->vnode && vf->vnode->mnt && vf->vnode->mnt->fs_data)
        bcache_sync((bcache_t *)vf->vnode->mnt->fs_data);
    return 0;
}

/* ============================================================
 * Directory / File management
 * ============================================================ */

int vfs_mkdir(const char *path, int mode) {
    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->cwd : "/";
    if (path[0] == '/') strncpy(resolved, path, MAX_PATH_LEN - 1);
    else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(resolved, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';
    size_t rlen = strlen(resolved);
    while (rlen > 1 && resolved[rlen - 1] == '/')
        resolved[--rlen] = '\0';

    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt || !mnt->root) return -ENOENT;

    const char *rel = strip_mount_prefix(resolved, mnt);
    char parent_path[MAX_PATH_LEN] = "";
    strncpy(parent_path, rel, MAX_PATH_LEN - 1);
    char *slash = strrchr(parent_path, '/');
    const char *name = slash ? slash + 1 : rel;
    if (slash) *slash = '\0';
    else parent_path[0] = '\0';

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
    int cmode = (mode & 07777) & ~(cur ? cur->umask : 022);
    int r = parent->ops->mkdir(parent, name, cmode);
    vnode_put(parent);
    return r;
}

int vfs_unlink(const char *path) {
    char resolved[MAX_PATH_LEN];
    task_t *cur = proc_current();
    const char *cwd = cur ? cur->cwd : "/";
    if (path[0] == '/') strncpy(resolved, path, MAX_PATH_LEN - 1);
    else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(resolved, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt || !mnt->root) return -ENOENT;

    const char *rel = strip_mount_prefix(resolved, mnt);
    char parent_path[MAX_PATH_LEN] = "";
    strncpy(parent_path, rel, MAX_PATH_LEN - 1);
    char *slash = strrchr(parent_path, '/');
    const char *name = slash ? slash + 1 : rel;
    if (slash) *slash = '\0';
    else parent_path[0] = '\0';

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
    int r = parent->ops->unlink(parent, name);
    vnode_put(parent);
    return r;
}

int vfs_rename(const char *old, const char *newpath) {
    if (!old || !newpath) return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->cwd : "/";

    char old_resolved[MAX_PATH_LEN];
    char new_resolved[MAX_PATH_LEN];
    if (old[0] == '/') strncpy(old_resolved, old, MAX_PATH_LEN - 1);
    else snprintf(old_resolved, MAX_PATH_LEN, "%s/%s", cwd, old);
    old_resolved[MAX_PATH_LEN - 1] = '\0';

    if (newpath[0] == '/') strncpy(new_resolved, newpath, MAX_PATH_LEN - 1);
    else snprintf(new_resolved, MAX_PATH_LEN, "%s/%s", cwd, newpath);
    new_resolved[MAX_PATH_LEN - 1] = '\0';

    char old_parent[MAX_PATH_LEN], old_name[MAX_NAME_LEN];
    char new_parent[MAX_PATH_LEN], new_name[MAX_NAME_LEN];

    char *slash = strrchr(old_resolved, '/');
    if (!slash) return -EINVAL;
    if (slash == old_resolved) { old_parent[0] = '/'; old_parent[1] = '\0'; }
    else {
        size_t plen = slash - old_resolved;
        memcpy(old_parent, old_resolved, plen);
        old_parent[plen] = '\0';
    }
    strncpy(old_name, slash + 1, MAX_NAME_LEN - 1);
    old_name[MAX_NAME_LEN - 1] = '\0';

    slash = strrchr(new_resolved, '/');
    if (!slash) return -EINVAL;
    if (slash == new_resolved) { new_parent[0] = '/'; new_parent[1] = '\0'; }
    else {
        size_t plen = slash - new_resolved;
        memcpy(new_parent, new_resolved, plen);
        new_parent[plen] = '\0';
    }
    strncpy(new_name, slash + 1, MAX_NAME_LEN - 1);
    new_name[MAX_NAME_LEN - 1] = '\0';

    mount_t *old_mnt = vfs_find_mount(old_parent);
    mount_t *new_mnt = vfs_find_mount(new_parent);
    if (!old_mnt || !new_mnt) return -ENOENT;
    if (old_mnt != new_mnt) return -EXDEV;

    vnode_t *old_dir = vnode_lookup_path(old_mnt->root, strip_mount_prefix(old_parent, old_mnt));
    vnode_t *new_dir = vnode_lookup_path(new_mnt->root, strip_mount_prefix(new_parent, new_mnt));
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
    int r = old_dir->ops->rename(old_dir, old_name, new_dir, new_name);
    vnode_put(old_dir);
    vnode_put(new_dir);
    return r;
}

int vfs_rmdir(const char *path) {
    if (!path) return -EINVAL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->cwd : "/";

    char resolved[MAX_PATH_LEN];
    if (path[0] == '/') strncpy(resolved, path, MAX_PATH_LEN - 1);
    else snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    resolved[MAX_PATH_LEN - 1] = '\0';

    char parent_path[MAX_PATH_LEN];
    char *slash = strrchr(resolved, '/');
    if (!slash) return -EINVAL;
    if (slash == resolved) { parent_path[0] = '/'; parent_path[1] = '\0'; }
    else {
        size_t plen = slash - resolved;
        memcpy(parent_path, resolved, plen);
        parent_path[plen] = '\0';
    }
    const char *name = slash + 1;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt || !mnt->root) return -ENOENT;

    vnode_t *parent = vnode_lookup_path(mnt->root, strip_mount_prefix(parent_path, mnt));
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
    int r = parent->ops->rmdir(parent, name);
    vnode_put(parent);
    return r;
}

static vnode_t *vfs_resolve_no_follow_final(const char *path) {
    if (!path || !*path) return NULL;

    task_t *cur = proc_current();
    const char *cwd = cur ? cur->cwd : "/";

    char resolved[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(resolved, path, MAX_PATH_LEN - 1);
    } else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(resolved, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    size_t len = strlen(resolved);
    while (len > 1 && resolved[len - 1] == '/')
        resolved[--len] = '\0';

    if (strcmp(resolved, "/") == 0)
        return vfs_resolve(resolved);

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) return NULL;
    if (last_slash == resolved) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = last_slash - resolved;
        memcpy(parent_path, resolved, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, last_slash + 1, MAX_NAME_LEN - 1);
    name[MAX_NAME_LEN - 1] = '\0';
    if (name[0] == '\0') return NULL;

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return NULL;
    const char *rel = strip_mount_prefix(parent_path, mnt);
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
    int r = parent->ops->lookup(parent, name, &vn);
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
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->vnode)
            return vfs_vnode_stat(vf->vnode, st);
        if (is_char_device_vfile(vf)) {
            fill_char_kstat(st);
            return 0;
        }
        if (is_pipe_vfile(vf)) {
            fill_pipe_kstat(st);
            return 0;
        }
    }
    return -EBADF;
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
        uint32_t uid = (flags & AT_EACCESS) ? (cur ? (uint32_t)cur->euid : 0)
                                            : (cur ? (uint32_t)cur->uid : 0);
        uint32_t gid = (flags & AT_EACCESS) ? (cur ? (uint32_t)cur->egid : 0)
                                            : (cur ? (uint32_t)cur->gid : 0);
        r = vfs_mode_has_perm_ids(st.st_mode, st.st_uid, st.st_gid, uid, gid, mode);
    }
    vnode_put(vn);
    return r;
}

static int vfs_chmod_vnode(vnode_t *vn, int mode) {
    if (!vn) return -ENOENT;
    if (!vfs_current_owns(vn)) return -EPERM;
    mode &= 07777;
    if (!vfs_task_is_root(proc_current())) {
        kstat_t st;
        if (vfs_vnode_stat(vn, &st) == 0 && !vfs_task_in_group(proc_current(), st.st_gid))
            mode &= ~S_ISGID;
    }
    if (vn->ops && vn->ops->chmod)
        return vn->ops->chmod(vn, mode);
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
    vfile_t *vf = vfs_get_file(fd);
    if (!vf || !vf->vnode) return -EBADF;
    return vfs_chmod_vnode(vf->vnode, mode);
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

    if (!vfs_task_is_root(cur)) {
        if (uid != -1 && (uint32_t)uid != st.st_uid)
            return -EPERM;
        if (gid != -1 && !((uint32_t)cur->fsuid == st.st_uid && vfs_task_in_group(cur, (uint32_t)gid)))
            return -EPERM;
    }
    if (vn->ops && vn->ops->chown)
        return vn->ops->chown(vn, uid, gid);
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
    vfile_t *vf = vfs_get_file(fd);
    if (!vf || !vf->vnode) return -EBADF;
    return vfs_chown_vnode(vf->vnode, uid, gid);
}

static vfs_time_meta_t *vfs_time_meta_for(vnode_t *vn)
{
    if (!vn) return NULL;
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (g_time_meta[i].used && g_time_meta[i].mnt == vn->mnt &&
            g_time_meta[i].ino == vn->ino)
            return &g_time_meta[i];
    }
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (!g_time_meta[i].used) {
            memset(&g_time_meta[i], 0, sizeof(g_time_meta[i]));
            g_time_meta[i].used = 1;
            g_time_meta[i].mnt = vn->mnt;
            g_time_meta[i].ino = vn->ino;
            return &g_time_meta[i];
        }
    }
    return NULL;
}

static void vfs_touch_mtime(vnode_t *vn)
{
    vfs_time_meta_t *tm = vfs_time_meta_for(vn);
    if (!tm) return;
    uint64_t now[2];
    timekeeping_get_realtime(now);
    if (tm->atime == 0 && tm->atime_nsec == 0) {
        tm->atime = now[0];
        tm->atime_nsec = now[1];
    }
    tm->mtime = now[0];
    tm->mtime_nsec = now[1];
    tm->ctime = now[0];
    tm->ctime_nsec = now[1];
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
    vfs_time_meta_t *tm = vfs_time_meta_for(vn);
    if (!tm) {
        vnode_put(vn);
        return -ENOSPC;
    }
    uint64_t now[2];
    timekeeping_get_realtime(now);
    uint64_t atime = now[0], atime_nsec = now[1];
    uint64_t mtime = now[0], mtime_nsec = now[1];
    if (times) {
        atime = times[0];
        atime_nsec = times[1];
        mtime = times[2];
        mtime_nsec = times[3];
    }
    if (atime_nsec >= 1000000000ULL || mtime_nsec >= 1000000000ULL) {
        vnode_put(vn);
        return -EINVAL;
    }
    tm->atime = atime;
    tm->atime_nsec = atime_nsec;
    tm->mtime = mtime;
    tm->mtime_nsec = mtime_nsec;
    tm->ctime = now[0];
    tm->ctime_nsec = now[1];
    vnode_put(vn);
    return 0;
}

int vfs_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    (void)dirfd;
    if (!path || !buf || sz == 0) return -EINVAL;
    char resolved[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(resolved, path, MAX_PATH_LEN - 1);
    } else {
        task_t *cur = proc_current();
        const char *cwd = cur ? cur->cwd : "/";
        snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

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
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == resolved) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = last_slash - resolved;
        memcpy(parent_path, resolved, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, last_slash + 1, MAX_NAME_LEN - 1);
    name[MAX_NAME_LEN - 1] = '\0';

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return -ENOENT;
    const char *rel = strip_mount_prefix(parent_path, mnt);
    vnode_t *parent = vnode_lookup_path(mnt->root, rel);
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOENT;
    }

    vnode_t *vn = NULL;
    if (parent->ops && parent->ops->lookup) {
        int r = parent->ops->lookup(parent, name, &vn);
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
    const char *cwd = cur ? cur->cwd : "/";
    if (newpath[0] == '/') strncpy(resolved, newpath, MAX_PATH_LEN - 1);
    else snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, newpath);
    resolved[MAX_PATH_LEN - 1] = '\0';

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) { vnode_put(target); return -EINVAL; }
    if (last_slash == resolved) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = last_slash - resolved;
        memcpy(parent_path, resolved, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, last_slash + 1, MAX_NAME_LEN - 1);
    name[MAX_NAME_LEN - 1] = '\0';
    if (name[0] == '\0') { vnode_put(target); return -ENOENT; }

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) { vnode_put(target); return -ENOENT; }
    if (target->mnt != mnt) { vnode_put(target); return -EXDEV; }

    vnode_t *parent = vnode_lookup_path(mnt->root, strip_mount_prefix(parent_path, mnt));
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
    return r;
}

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -EINVAL;

    char resolved[MAX_PATH_LEN];
    if (linkpath[0] == '/') {
        strncpy(resolved, linkpath, MAX_PATH_LEN - 1);
    } else {
        task_t *cur = proc_current();
        const char *cwd = cur ? cur->cwd : "/";
        snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, linkpath);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    char parent_path[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) return -EINVAL;
    if (last_slash == resolved) {
        strcpy(parent_path, "/");
    } else {
        size_t plen = last_slash - resolved;
        memcpy(parent_path, resolved, plen);
        parent_path[plen] = '\0';
    }
    strncpy(name, last_slash + 1, MAX_NAME_LEN - 1);
    name[MAX_NAME_LEN - 1] = '\0';

    mount_t *mnt = vfs_find_mount(parent_path);
    if (!mnt) return -ENOENT;
    const char *rel = strip_mount_prefix(parent_path, mnt);
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
    return r;
}

int vfs_chdir(const char *path) {
    task_t *cur = proc_current();
    if (!cur) return -EINVAL;

    char resolved[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(resolved, path, MAX_PATH_LEN - 1);
    } else {
        const char *cwd = cur->cwd[0] ? cur->cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(resolved, MAX_PATH_LEN, "/%s", path);
        else
            snprintf(resolved, MAX_PATH_LEN, "%s/%s", cwd, path);
    }
    resolved[MAX_PATH_LEN - 1] = '\0';

    char parts[64][MAX_NAME_LEN];
    int depth = 0;
    char *p = resolved;
    while (*p == '/') p++;
    while (*p) {
        char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.')) {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (depth > 0) depth--;
        } else if (depth < 64) {
            if (len >= MAX_NAME_LEN)
                return -ENAMETOOLONG;
            size_t n = len < MAX_NAME_LEN ? len : MAX_NAME_LEN - 1;
            memcpy(parts[depth], p, n);
            parts[depth][n] = '\0';
            depth++;
        }
        if (!end) break;
        p = end + 1;
        while (*p == '/') p++;
    }

    char canon[MAX_PATH_LEN] = "/";
    for (int i = 0; i < depth; i++) {
        if (strlen(canon) + strlen(parts[i]) + 2 >= MAX_PATH_LEN)
            return -ENAMETOOLONG;
        if (strcmp(canon, "/") != 0) strcat(canon, "/");
        strcat(canon, parts[i]);
    }

    vnode_t *vn = vfs_resolve(canon);
    if (!vn) return g_lookup_errno ? g_lookup_errno : -ENOENT;
    if (vn->type != VFS_FT_DIR) { vnode_put(vn); return -ENOTDIR; }
    if (vfs_vnode_permission(vn, X_OK) < 0) { vnode_put(vn); return -EACCES; }
    vnode_put(vn);
    strncpy(cur->cwd, canon, MAX_PATH_LEN - 1);
    cur->cwd[MAX_PATH_LEN - 1] = '\0';
    return 0;
}

int vfs_getcwd(char *buf, size_t size) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (size < len) return -ERANGE;
    memcpy(buf, cwd, len);
    return 0;
}

/* ============================================================
 * Pipe
 * ============================================================ */

typedef struct pipe_buf {
    char    *data;
    size_t   capacity;
    size_t   head, tail, used;
    size_t   logical_size;
    int      writer_closed;
    int      reader_closed;
    int      ref;
} pipe_buf_t;

static int pipe_read(vfile_t *vf, char *buf, size_t count) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;
    while (pb->used == 0) {
        if (pb->writer_closed) return 0; /* EOF */
        if (vf->flags & O_NONBLOCK) return -EAGAIN;
        proc_yield();
    }
    size_t n = pb->used < count ? pb->used : count;
    for (size_t i = 0; i < n; i++) {
        buf[i] = pb->data[pb->tail];
        pb->tail = (pb->tail + 1) % pb->capacity;
        pb->used--;
    }
    return (int)n;
}

static int pipe_write(vfile_t *vf, const char *buf, size_t count) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;
    if (pb->reader_closed) return -EPIPE;
    size_t n = 0;
    while (n < count) {
        while (pb->used == pb->capacity) {
            if (pb->reader_closed) return n ? (int)n : -EPIPE;
            if (vf->flags & O_NONBLOCK) return n ? (int)n : -EAGAIN;
            proc_yield();
        }
        pb->data[pb->head] = buf[n++];
        pb->head = (pb->head + 1) % pb->capacity;
        pb->used++;
    }
    return (int)n;
}

static int pipe_null_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count;
    return 0;
}

static int pipe_null_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf; (void)buf;
    return (int)count;
}

static int pipe_resize(pipe_buf_t *pb, size_t new_capacity) {
    if (!pb) return -EINVAL;
    if (new_capacity < PIPE_BUF_SIZE)
        new_capacity = PIPE_BUF_SIZE;
    if (new_capacity < pb->used)
        return -EBUSY;
    if (new_capacity == pb->capacity) {
        pb->logical_size = new_capacity;
        return (int)new_capacity;
    }

    char *new_data = (char *)kmalloc(new_capacity);
    if (!new_data)
        return -ENOMEM;
    for (size_t i = 0; i < pb->used; i++)
        new_data[i] = pb->data[(pb->tail + i) % pb->capacity];
    kfree(pb->data);
    pb->data = new_data;
    pb->capacity = new_capacity;
    pb->logical_size = new_capacity;
    pb->tail = 0;
    pb->head = pb->used % pb->capacity;
    return (int)new_capacity;
}

static int pipe_read_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        pb->reader_closed = 1;
        pb->ref--;
        if (!pb->ref) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
    }
    return 0;
}

static int pipe_write_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        pb->writer_closed = 1;
        pb->ref--;
        if (!pb->ref) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
    }
    return 0;
}

static vfile_ops_t g_pipe_read_ops  = { .read = pipe_read,       .write = pipe_null_write, .close = pipe_read_close  };
static vfile_ops_t g_pipe_write_ops = { .read = pipe_null_read,  .write = pipe_write,      .close = pipe_write_close };

int vfs_poll_events(int fd, short events) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return POLLNVAL;

    int nr = net_poll_events(fd, events);
    if (nr >= 0) return nr;

    short revents = 0;
    if (is_pipe_vfile(vf)) {
        pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
        if (!pb) return POLLNVAL;
        if (vf->ops == &g_pipe_read_ops) {
            if ((events & POLLIN) && (pb->used > 0 || pb->writer_closed))
                revents |= POLLIN;
            if (pb->writer_closed)
                revents |= POLLHUP;
        } else {
            if ((events & POLLOUT) && pb->used < pb->capacity && !pb->reader_closed)
                revents |= POLLOUT;
            if (pb->reader_closed)
                revents |= POLLERR;
        }
        return revents;
    }

    if (is_char_device_vfile(vf)) {
        extern int uart_has_input(void);
        if ((events & POLLIN) && (fd != 0 || uart_has_input()))
            revents |= POLLIN;
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

int vfs_pipe(int pipefd[2]) {
    pipe_buf_t *pb = (pipe_buf_t *)kmalloc(sizeof(pipe_buf_t));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(*pb));
    pb->ref = 2;
    task_t *cur = proc_current();
    pb->logical_size = (cur && cur->euid == 0) ? PIPE_DEFAULT_SIZE : PIPE_BUF_SIZE;
    pb->capacity = pb->logical_size;
    pb->data = (char *)kmalloc(pb->capacity);
    if (!pb->data) { kfree(pb); return -ENOMEM; }

    vfile_t *rd = (vfile_t *)kmalloc(sizeof(vfile_t));
    vfile_t *wr = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!rd || !wr) {
        kfree(pb->data);
        kfree(pb);
        if (rd) kfree(rd);
        if (wr) kfree(wr);
        return -ENOMEM;
    }

    memset(rd, 0, sizeof(*rd)); rd->ops = &g_pipe_read_ops;  rd->priv = pb; rd->flags = O_RDONLY; rd->ref_count = 1;
    memset(wr, 0, sizeof(*wr)); wr->ops = &g_pipe_write_ops; wr->priv = pb; wr->flags = O_WRONLY; wr->ref_count = 1;

    int fdrd = vfs_alloc_fd(rd);
    int fdwr = vfs_alloc_fd(wr);
    if (fdrd < 0 || fdwr < 0) {
        if (fdrd >= 0) vfs_close(fdrd);
        else kfree(rd);
        if (fdwr >= 0) vfs_close(fdwr);
        else kfree(wr);
        if (pb->ref > 0) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
        return -EMFILE;
    }
    pipefd[0] = fdrd;
    pipefd[1] = fdwr;
    return 0;
}

/* ============================================================
 * dup / dup3 / fcntl
 * ============================================================ */

typedef struct vfs_flock {
    short l_type;
    short l_whence;
    int64_t l_start;
    int64_t l_len;
    int l_pid;
} vfs_flock_t;

typedef struct vfs_fowner {
    int type;
    int pid;
} vfs_fowner_t;

typedef struct vfs_file_lock {
    int used;
    int owner_kind;
    uintptr_t key;
    int owner;
    short type;
    int64_t start;
    int64_t end;
} vfs_file_lock_t;

typedef struct vfs_bsd_flock {
    int used;
    uintptr_t key;
    vfile_t *owner;
    int type;
} vfs_bsd_flock_t;

#define VFS_FILE_LOCK_MAX 256
#define VFS_LOCK_OWNER_PID 1
#define VFS_LOCK_OWNER_OFD 2
static vfs_file_lock_t g_file_locks[VFS_FILE_LOCK_MAX];
static vfs_bsd_flock_t g_bsd_flocks[VFS_FILE_LOCK_MAX];
static spinlock_t g_file_lock_table_lock = SPINLOCK_INIT;

static uintptr_t vfs_lock_key(vfile_t *vf) {
    if (vf && vf->vnode && vf->vnode->ino)
        return (((uintptr_t)vf->vnode->mnt) >> 3) ^
               ((uintptr_t)vf->vnode->ino << 17) ^
               (uintptr_t)vf->vnode->ino;
    return (uintptr_t)vf;
}

static int64_t vfs_file_size(vfile_t *vf) {
    if (!vf || !vf->vnode) return 0;
    kstat_t st;
    memset(&st, 0, sizeof(st));
    if (vf->vnode->ops && vf->vnode->ops->stat &&
        vf->vnode->ops->stat(vf->vnode, &st) == 0)
        return (int64_t)st.st_size;
    return (int64_t)vf->vnode->size;
}

static int vfs_lock_range(vfile_t *vf, const vfs_flock_t *lk,
                          int64_t *start, int64_t *end) {
    int64_t base;
    if (!vf || !lk || !start || !end) return -EINVAL;
    if (lk->l_whence == SEEK_SET) base = 0;
    else if (lk->l_whence == SEEK_CUR) base = (int64_t)vf->offset;
    else if (lk->l_whence == SEEK_END) base = vfs_file_size(vf);
    else return -EINVAL;

    int64_t s = base + lk->l_start;
    int64_t e;
    if (lk->l_len == 0) {
        e = 0x7fffffffffffffffLL;
    } else if (lk->l_len > 0) {
        e = s + lk->l_len - 1;
    } else {
        e = s - 1;
        s = s + lk->l_len;
    }
    if (s < 0) return -EINVAL;
    *start = s;
    *end = e;
    return 0;
}

static int vfs_lock_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
    return a0 <= b1 && b0 <= a1;
}

static int vfs_lock_conflicts(const vfs_file_lock_t *held, uintptr_t key,
                              int owner_kind, int owner, short type,
                              int64_t start, int64_t end) {
    if (!held->used || held->key != key)
        return 0;
    if (held->owner_kind == owner_kind && held->owner == owner)
        return 0;
    if (!vfs_lock_overlaps(held->start, held->end, start, end))
        return 0;
    if (held->type == F_RDLCK && type == F_RDLCK)
        return 0;
    return 1;
}

static int vfs_fcntl_getlk(vfile_t *vf, long arg, int owner_kind, int owner) {
    if (!arg) return -EFAULT;
    vfs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    if (lk.l_type != F_RDLCK && lk.l_type != F_WRLCK && lk.l_type != F_UNLCK)
        return -EINVAL;

    int64_t start, end;
    int r = vfs_lock_range(vf, &lk, &start, &end);
    if (r < 0) return r;
    uintptr_t key = vfs_lock_key(vf);

    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!vfs_lock_conflicts(&g_file_locks[i], key, owner_kind, owner,
                                lk.l_type, start, end))
            continue;
        lk.l_type = g_file_locks[i].type;
        lk.l_whence = SEEK_SET;
        lk.l_start = g_file_locks[i].start;
        lk.l_len = (g_file_locks[i].end == 0x7fffffffffffffffLL) ?
                   0 : (g_file_locks[i].end - g_file_locks[i].start + 1);
        lk.l_pid = g_file_locks[i].owner;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return copy_to_user((void *)arg, &lk, sizeof(lk)) < 0 ? -EFAULT : 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);

    lk.l_type = F_UNLCK;
    lk.l_pid = owner;
    return copy_to_user((void *)arg, &lk, sizeof(lk)) < 0 ? -EFAULT : 0;
}

static int vfs_fcntl_setlk(vfile_t *vf, long arg, int owner_kind, int owner, int wait) {
    if (!arg) return -EFAULT;
    vfs_flock_t lk;
    if (copy_from_user(&lk, (void *)arg, sizeof(lk)) < 0) return -EFAULT;
    if (lk.l_type != F_RDLCK && lk.l_type != F_WRLCK && lk.l_type != F_UNLCK)
        return -EINVAL;

    int64_t start, end;
    int r = vfs_lock_range(vf, &lk, &start, &end);
    if (r < 0) return r;
    uintptr_t key = vfs_lock_key(vf);

retry:
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!vfs_lock_conflicts(&g_file_locks[i], key, owner_kind, owner,
                                lk.l_type, start, end))
            continue;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        if (!wait) return -EAGAIN;
        proc_yield();
        goto retry;
    }

    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!g_file_locks[i].used || g_file_locks[i].key != key ||
            g_file_locks[i].owner_kind != owner_kind ||
            g_file_locks[i].owner != owner)
            continue;
        if (vfs_lock_overlaps(g_file_locks[i].start, g_file_locks[i].end, start, end))
            g_file_locks[i].used = 0;
    }

    if (lk.l_type == F_UNLCK) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }

    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!g_file_locks[i].used) {
            g_file_locks[i].used = 1;
            g_file_locks[i].owner_kind = owner_kind;
            g_file_locks[i].key = key;
            g_file_locks[i].owner = owner;
            g_file_locks[i].type = lk.l_type;
            g_file_locks[i].start = start;
            g_file_locks[i].end = end;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    return -ENOLCK;
}

void vfs_release_process_locks(int pid) {
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == VFS_LOCK_OWNER_PID &&
            g_file_locks[i].owner == pid)
            g_file_locks[i].used = 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
}

static void vfs_release_open_file_locks(vfile_t *vf, int gfd) {
    uintptr_t key = vfs_lock_key(vf);
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == VFS_LOCK_OWNER_OFD &&
            g_file_locks[i].owner == gfd)
            g_file_locks[i].used = 0;
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf)
            g_bsd_flocks[i].used = 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
}

int vfs_flock(int fd, int operation) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return -EBADF;

    int op = operation & (LOCK_SH | LOCK_EX | LOCK_UN);
    if ((operation & ~(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN)) || op == 0)
        return -EINVAL;
    if ((op & (op - 1)) != 0)
        return -EINVAL;

    uintptr_t key = vfs_lock_key(vf);
    if (op == LOCK_UN) {
        uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
        for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
            if (g_bsd_flocks[i].used &&
                g_bsd_flocks[i].key == key &&
                g_bsd_flocks[i].owner == vf)
                g_bsd_flocks[i].used = 0;
        }
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }

    int type = (op == LOCK_EX) ? F_WRLCK : F_RDLCK;

retry:
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used || g_bsd_flocks[i].key != key ||
            g_bsd_flocks[i].owner == vf)
            continue;
        if (g_bsd_flocks[i].type == F_RDLCK && type == F_RDLCK)
            continue;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        if (operation & LOCK_NB) return -EAGAIN;
        proc_yield();
        goto retry;
    }

    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf) {
            g_bsd_flocks[i].type = type;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }

    for (int i = 0; i < VFS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used) {
            g_bsd_flocks[i].used = 1;
            g_bsd_flocks[i].key = key;
            g_bsd_flocks[i].owner = vf;
            g_bsd_flocks[i].type = type;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    return -ENOLCK;
}

static int vfs_dupfd(int fd, int minfd) {
    if (minfd < 0) minfd = 0;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    if (fd < 0 || fd >= GFILE_MAX || !g_files[fd]) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBADF;
    }
    vfile_t *vf = g_files[fd];
    vf->ref_count++;
    for (int i = minfd; i < GFILE_MAX; i++) {
        if (!g_files[i]) {
            g_files[i] = vf;
            spin_unlock_irqrestore(&g_file_lock, flags);
            return i;
        }
    }
    vf->ref_count--;
    spin_unlock_irqrestore(&g_file_lock, flags);
    return -EMFILE;
}

int vfs_dup(int fd) {
    return vfs_dupfd(fd, 3);
}

int vfs_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    if (newfd >= GFILE_MAX || newfd < 0) return -EBADF;
    if (oldfd == newfd) return -EINVAL;
    if (g_files[newfd]) vfs_close(newfd);
    uint64_t irqflags = spin_lock_irqsave(&g_file_lock);
    if (oldfd >= 0 && oldfd < GFILE_MAX && g_files[oldfd]) {
        g_files[newfd] = g_files[oldfd];
        g_files[newfd]->ref_count++;
        spin_unlock_irqrestore(&g_file_lock, irqflags);
        return newfd;
    }
    spin_unlock_irqrestore(&g_file_lock, irqflags);
    return -EBADF;
}

int vfs_fcntl(int fd, int cmd, long arg) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return -EBADF;
    task_t *t = proc_current();
    int owner = t ? t->pid : 0;

    if (cmd == F_GETFL)
        return vf->flags;
    if (cmd == F_SETFL) {
        int accmode = vf->flags & O_ACCMODE;
        vf->flags = accmode | ((int)arg & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC));
        return 0;
    }
    if (cmd == F_DUPFD)
        return vfs_dupfd(fd, (int)arg);
    if (cmd == F_DUPFD_CLOEXEC)
        return vfs_dupfd(fd, (int)arg);
    if (cmd == F_GETFD || cmd == F_SETFD)
        return 0;

    if (cmd == F_GETLK)
        return vfs_fcntl_getlk(vf, arg, VFS_LOCK_OWNER_PID, owner);
    if (cmd == F_SETLK)
        return vfs_fcntl_setlk(vf, arg, VFS_LOCK_OWNER_PID, owner, 0);
    if (cmd == F_SETLKW)
        return vfs_fcntl_setlk(vf, arg, VFS_LOCK_OWNER_PID, owner, 1);
    if (cmd == F_OFD_GETLK)
        return vfs_fcntl_getlk(vf, arg, VFS_LOCK_OWNER_OFD, fd);
    if (cmd == F_OFD_SETLK)
        return vfs_fcntl_setlk(vf, arg, VFS_LOCK_OWNER_OFD, fd, 0);
    if (cmd == F_OFD_SETLKW)
        return vfs_fcntl_setlk(vf, arg, VFS_LOCK_OWNER_OFD, fd, 1);

    if (cmd == F_SETOWN) {
        vf->owner_type = arg < 0 ? F_OWNER_PGRP : F_OWNER_PID;
        vf->owner_pid = arg < 0 ? -(int)arg : (int)arg;
        return 0;
    }
    if (cmd == F_GETOWN)
        return vf->owner_type == F_OWNER_PGRP ? -vf->owner_pid : vf->owner_pid;
    if (cmd == F_SETOWN_EX) {
        if (!arg) return -EFAULT;
        vfs_fowner_t own;
        if (copy_from_user(&own, (void *)arg, sizeof(own)) < 0) return -EFAULT;
        if (own.type < F_OWNER_TID || own.type > F_OWNER_PGRP || own.pid < 0)
            return -EINVAL;
        vf->owner_type = own.type;
        vf->owner_pid = own.pid;
        return 0;
    }
    if (cmd == F_GETOWN_EX) {
        if (!arg) return -EFAULT;
        vfs_fowner_t own = {
            .type = vf->owner_type ? vf->owner_type : F_OWNER_PID,
            .pid = vf->owner_pid,
        };
        return copy_to_user((void *)arg, &own, sizeof(own)) < 0 ? -EFAULT : 0;
    }
    if (cmd == F_SETSIG) {
        vf->owner_signal = (int)arg;
        return 0;
    }
    if (cmd == F_GETSIG)
        return vf->owner_signal;
    if (cmd == F_GETOWNER_UIDS) {
        if (!arg) return -EFAULT;
        int ids[2] = { t ? t->uid : 0, t ? t->euid : 0 };
        return copy_to_user((void *)arg, ids, sizeof(ids)) < 0 ? -EFAULT : 0;
    }

    if (cmd == F_GETPIPE_SZ) {
        if (!is_pipe_vfile(vf)) return -EINVAL;
        pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
        return pb && pb->logical_size ? (int)pb->logical_size : PIPE_BUF_SIZE;
    }
    if (cmd == F_SETPIPE_SZ) {
        if (!is_pipe_vfile(vf)) return -EINVAL;
        if (arg <= 0) return -EINVAL;
        pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
        if (!pb) return -EINVAL;
        return pipe_resize(pb, (size_t)arg);
    }

    if (cmd == F_GET_SEALS)
        return vf->seals;
    if (cmd == F_ADD_SEALS) {
        if (vf->seals & F_SEAL_SEAL) return -EPERM;
        vf->seals |= (int)arg & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW |
                                 F_SEAL_WRITE | F_SEAL_FUTURE_WRITE);
        return 0;
    }

    if (cmd == F_GET_RW_HINT || cmd == F_GET_FILE_RW_HINT) {
        if (!arg) return -EFAULT;
        return copy_to_user((void *)arg, &vf->rw_hint, sizeof(vf->rw_hint)) < 0 ? -EFAULT : 0;
    }
    if (cmd == F_SET_RW_HINT || cmd == F_SET_FILE_RW_HINT) {
        if (!arg) return -EFAULT;
        uint64_t hint;
        if (copy_from_user(&hint, (void *)arg, sizeof(hint)) < 0) return -EFAULT;
        vf->rw_hint = hint;
        return 0;
    }

    if (cmd == F_SETLEASE || cmd == F_NOTIFY || cmd == F_CANCELLK)
        return 0;
    if (cmd == F_GETLEASE)
        return F_UNLCK;

    return -EINVAL;
}

/* ============================================================
 * VFS Mount
 * ============================================================ */

int vfs_mount(const char *dev, const char *path, const char *fstype, int flags) {
    (void)dev;
    if (!path || !fstype) return -EINVAL;
    for (int i = 0; i < g_nmounts; i++) {
        if (strcmp(g_mounts[i].path, path) == 0) {
            g_mounts[i].flags = flags;
            return 0;
        }
    }
    if (g_nmounts >= MAX_MOUNTS) return -ENOMEM;
    if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0) {
        vnode_t *target = vfs_resolve(path);
        if (!target) return -ENOENT;
        int is_dir = target->type == VFS_FT_DIR;
        vnode_put(target);
        if (!is_dir) return -ENOTDIR;

        mount_t *mnt = &g_mounts[g_nmounts++];
        memset(mnt, 0, sizeof(*mnt));
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type = FS_TYPE_RAMFS;
        mnt->flags = flags;
        mnt->root = ramfs_mount_empty(mnt);
        if (!mnt->root) {
            g_nmounts--;
            return -ENOMEM;
        }
        mnt->root->mnt = mnt;
        return 0;
    }
    printf("[VFS] vfs_mount: use vfs_mount_bc() for block filesystems\n");
    return -EINVAL;
}

int vfs_mount_bc(const char *path, const char *fstype, bcache_t *bc) {
    if (g_nmounts >= MAX_MOUNTS) return -ENOMEM;

    if (strcmp(fstype, "fat32") == 0 || strcmp(fstype, "vfat") == 0) {
        if (!bc) { printf("[VFS] No bcache for FAT32 mount\n"); return -ENODEV; }

        vnode_t *root = fat32_mount(bc);
        if (!root) return -EIO;

        mount_t *mnt = &g_mounts[g_nmounts++];
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_FAT32;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;

        printf("[VFS] Mounted FAT32 at %s\n", path);
        return 0;
    }

    if (strcmp(fstype, "ext4") == 0) {
        if (!bc) { printf("[VFS] No bcache for ext4 mount\n"); return -ENODEV; }

        vnode_t *root = ext4_mount(bc);
        if (!root) return -EIO;

        mount_t *mnt = &g_mounts[g_nmounts++];
        strncpy(mnt->path, path, MAX_PATH_LEN - 1);
        mnt->path[MAX_PATH_LEN - 1] = '\0';
        mnt->type  = FS_TYPE_EXT4;
        mnt->root  = root;
        mnt->fs_data = bc;

        root->mnt = mnt;

        printf("[VFS] Mounted ext4 at %s\n", path);
        return 0;
    }

    printf("[VFS] Unknown fstype: %s\n", fstype);
    return -EINVAL;
}

int vfs_umount(const char *path) {
    for (int i = 0; i < g_nmounts; i++) {
        if (strcmp(g_mounts[i].path, path) == 0) {
            if (g_mounts[i].type == FS_TYPE_FAT32) {
                fat32_unmount(g_mounts[i].root);
            } else if (g_mounts[i].type == FS_TYPE_EXT4) {
                ext4_unmount(g_mounts[i].root);
            }
            /* Compact mount table */
            for (int j = i; j < g_nmounts - 1; j++) g_mounts[j] = g_mounts[j + 1];
            g_nmounts--;
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
    vnode_put(vn);
    return r;
}

int vfs_ftruncate(int fd, size_t size) {
    vfile_t *vf = vfs_get_file(fd);
    if (!vf) return -EBADF;
    if (!vfs_should_write(vf->flags)) return -EINVAL;
    if (!vf->vnode || !vf->vnode->ops || !vf->vnode->ops->truncate)
        return -EINVAL;
    if ((vf->seals & F_SEAL_SHRINK) && size < vf->vnode->size) return -EPERM;
    if ((vf->seals & F_SEAL_GROW) && size > vf->vnode->size) return -EPERM;
    if (vf->seals & F_SEAL_WRITE) return -EPERM;
    return vf->vnode->ops->truncate(vf->vnode, size);
}

/* ============================================================
 * VFS init — set up std streams, root ramfs mount
 * ============================================================ */

void vfs_init(void) {
    spin_init(&g_file_lock);
    memset(g_files, 0, sizeof(g_files));
    memset(g_mounts, 0, sizeof(g_mounts));
    memset(g_file_locks, 0, sizeof(g_file_locks));
    memset(g_bsd_flocks, 0, sizeof(g_bsd_flocks));
    g_nmounts = 0;

    /* Register ramfs as root "/" mount */
    mount_t *mnt = &g_mounts[g_nmounts++];
    memset(mnt, 0, sizeof(*mnt));
    strcpy(mnt->path, "/");
    mnt->type = FS_TYPE_RAMFS;
    mnt->root = ramfs_mount(mnt);

    printf("[VFS] Initialized (root=ramfs)\n");

    vfs_mkdir("/dev", 0755);
    mnt = &g_mounts[g_nmounts++];
    memset(mnt, 0, sizeof(*mnt));
    strcpy(mnt->path, "/dev");
    mnt->type = FS_TYPE_DEVFS;
    mnt->root = devfs_mount();
    if (mnt->root) mnt->root->mnt = mnt;

    /* Install std streams at global fds 0,1,2 */
    g_files[STDIN_FILENO]  = devfs_create_stdio(STDIN_FILENO);
    g_files[STDOUT_FILENO] = devfs_create_stdio(STDOUT_FILENO);
    g_files[STDERR_FILENO] = devfs_create_stdio(STDERR_FILENO);

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
            mount_t *mnt = &g_mounts[g_nmounts++];
            memset(mnt, 0, sizeof(*mnt));
            strcpy(mnt->path, "/proc");
            mnt->type = FS_TYPE_PROCFS;
            mnt->root = procfs_root;
            procfs_root->mnt = mnt;
            printf("[VFS] Mounted procfs at /proc\n");
        }
    }
}
