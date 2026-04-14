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

#include "vfs.h"
#include "fs.h"
#include "fat32.h"
#include "ext4.h"
#include "mm.h"
#include "proc.h"
#include "string.h"
#include "stdio.h"
#include "panic.h"
#include "consts.h"
#include "defs.h"
#include "klog.h"
#include "virtio_blk.h"
#include "block_cache.h"

/* ============================================================
 * Global open-file table
 * ============================================================ */
#define GFILE_MAX   VFS_MAX_OPEN

static vfile_t *g_files[GFILE_MAX];

/* Mount table (simple linear) */
#define MAX_MOUNTS  8
static mount_t g_mounts[MAX_MOUNTS];
static int     g_nmounts = 0;

/* ---- File descriptor allocation ---- */

int vfs_alloc_fd(vfile_t *vf) {
    /* Find slot in global file table */
    int gfd = -1;
    for (int i = 3; i < GFILE_MAX; i++) { /* 0,1,2 reserved for std??? */
        if (!g_files[i]) { g_files[i] = vf; gfd = i; break; }
    }
    return gfd;
}

static void vfs_free_gfd(int gfd) {
    if (gfd >= 0 && gfd < GFILE_MAX) g_files[gfd] = NULL;
}

vfile_t *vfs_get_file(int fd) {
    if (fd < 0 || fd >= GFILE_MAX) return NULL;
    return g_files[fd];
}

/* ============================================================
 * Per-process fd table
 * We map per-process fds → global gfds
 * For simplicity, the per-process fd IS the global gfd.
 * ============================================================ */

void vfs_proc_init_fds(int *fd_table) {
    for (int i = 0; i < MAX_FILES; i++) fd_table[i] = -1;
    /* Allocate stdin/stdout/stderr as devfs entries on first init */
    /* These are special: fd 0,1,2 map to devfs uart entries */
    fd_table[0] = 0;
    fd_table[1] = 1;
    fd_table[2] = 2;
}

void vfs_proc_copy_fds(const int *src, int *dst) {
    for (int i = 0; i < MAX_FILES; i++) {
        dst[i] = src[i];
        if (src[i] >= 0 && src[i] < GFILE_MAX && g_files[src[i]]) {
            g_files[src[i]]->ref_count++;
        }
    }
}

void vfs_proc_close_all_fds(int *fd_table) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (fd_table[i] >= 3) {
            vfs_close(fd_table[i]);
        }
        fd_table[i] = -1;
    }
}

/* ============================================================
 * UART / Devfs special file operations
 * fd 0,1,2 = stdin/stdout/stderr via UART
 * ============================================================ */

extern void uart_putc(char c);
extern int  uart_getc(void);
extern int  uart_try_getc(void);

static int devfs_stdin_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    if (count == 0) return 0;
    int c = uart_getc();
    if (c < 0) return 0;
    buf[0] = (char)c;
    return 1;
}

static int devfs_stdout_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf;
    for (size_t i = 0; i < count; i++) uart_putc(buf[i]);
    return (int)count;
}

static int devfs_null_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count; return 0;
}

static int devfs_null_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf; (void)buf; return (int)count;
}

static int devfs_zero_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; memset(buf, 0, count); return (int)count;
}

static vfile_ops_t g_stdin_ops  = { .read = devfs_stdin_read, .write = devfs_stdout_write };
static vfile_ops_t g_stdout_ops = { .read = devfs_null_read,  .write = devfs_stdout_write };
static vfile_ops_t g_stderr_ops = { .read = devfs_null_read,  .write = devfs_stdout_write };
static vfile_ops_t g_null_ops   = { .read = devfs_null_read,  .write = devfs_null_write   };
static vfile_ops_t g_zero_ops   = { .read = devfs_zero_read,  .write = devfs_null_write   };

static vfile_t g_stdin_file  = { .ref_count = 999, .ops = &g_stdin_ops  };
static vfile_t g_stdout_file = { .ref_count = 999, .ops = &g_stdout_ops };
static vfile_t g_stderr_file = { .ref_count = 999, .ops = &g_stderr_ops };

/* ============================================================
 * RAMFS — VFS Bridge
 * Bridges the legacy fs.c into the modern vnode system.
 * ============================================================ */

/* Forward declarations for ops */
static vnode_ops_t g_ramfs_vnode_ops;
static vfile_ops_t g_ramfs_fops;

static vnode_t *ramfs_make_vnode(mount_t *mnt, inode_t *inode) {
    if (!inode) return NULL;
    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino        = (uint64_t)inode->inum;
    vn->type       = (inode->type == FT_DIRECTORY) ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode       = (inode->type == FT_DIRECTORY) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    vn->size       = inode->size;
    vn->ref_count  = 1;
    vn->mnt        = mnt;
    vn->fs_data    = (void *)inode;
    vn->ops        = &g_ramfs_vnode_ops;
    /* parent will be set during lookup if needed */
    return vn;
}

static int ramfs_vnode_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *found = NULL;
    int r = fs_inode_lookup(dinode, name, &found);
    if (r < 0) return r;
    *out = ramfs_make_vnode(dir->mnt, found);
    if (*out) (*out)->parent = dir;
    return (*out) ? 0 : -ENOMEM;
}

static int ramfs_vnode_stat(vnode_t *vn, kstat_t *st) {
    inode_t *inode = (inode_t *)vn->fs_data;
    memset(st, 0, sizeof(*st));
    st->st_ino  = inode->inum;
    st->st_mode = vn->mode;
    st->st_size = inode->size;
    st->st_nlink = 1;
    return 0;
}

static int ramfs_vnode_mkdir(vnode_t *dir, const char *name, int mode) {
    (void)mode;
    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *child = alloc_inode(FT_DIRECTORY);
    if (!child) return -ENOMEM;
    child->parent = dinode;
    child->capacity = 64 * sizeof(dir_entry_t);
    child->data = kmalloc(child->capacity);
    if (!child->data) { child->ref_count = 0; return -ENOMEM; }
    memset(child->data, 0, child->capacity);
    add_dir_entry(child, ".", child->inum);
    add_dir_entry(child, "..", dinode->inum);
    add_dir_entry(dinode, name, child->inum);
    return 0;
}

static int ramfs_vnode_create(vnode_t *dir, const char *name, int mode, vnode_t **out) {
    (void)mode;
    inode_t *dinode = (inode_t *)dir->fs_data;
    inode_t *child = alloc_inode(FT_REGULAR);
    if (!child) return -ENOMEM;
    child->parent = dinode;
    child->capacity = 4096;
    child->data = kmalloc(child->capacity);
    if (!child->data) { child->ref_count = 0; return -ENOMEM; }
    add_dir_entry(dinode, name, child->inum);
    *out = ramfs_make_vnode(dir->mnt, child);
    if (*out) (*out)->parent = dir;
    return *out ? 0 : -ENOMEM;
}

static void ramfs_vnode_release(vnode_t *vn) {
    kfree(vn);
}

static int ramfs_vnode_unlink(vnode_t *dir, const char *name) {
    inode_t *dinode = (inode_t *)dir->fs_data;
    dir_entry_t *entries = (dir_entry_t *)dinode->data;
    int n_entries = dinode->size / sizeof(dir_entry_t);

    for (int i = 0; i < n_entries; i++) {
        if (entries[i].name[0] != '\0' && strcmp(entries[i].name, name) == 0) {
            entries[i].name[0] = '\0';
            return 0;
        }
    }
    return -ENOENT;
}

static vnode_ops_t g_ramfs_vnode_ops = {
    .lookup   = ramfs_vnode_lookup,
    .stat     = ramfs_vnode_stat,
    .release  = ramfs_vnode_release,
    .mkdir    = ramfs_vnode_mkdir,
    .create   = ramfs_vnode_create,
    .unlink   = ramfs_vnode_unlink,
};

/* File operations */
static int ramfs_fread(vfile_t *vf, char *buf, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (vf->offset >= inode->size) return 0;
    size_t avail = inode->size - vf->offset;
    size_t n = count < avail ? count : avail;
    if (n > 0) {
        memcpy(buf, inode->data + vf->offset, n);
        vf->offset += n;
    }
    return (int)n;
}

static int ramfs_fwrite(vfile_t *vf, const char *buf, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (inode->type == FT_DIRECTORY) return -EISDIR;

    size_t needed = vf->offset + count;
    if (needed > inode->capacity) {
        size_t new_cap = needed * 2;
        char *new_data = (char *)kmalloc(new_cap);
        if (!new_data) return -ENOMEM;
        if (inode->data) {
            memcpy(new_data, inode->data, inode->size);
            kfree(inode->data);
        }
        memset(new_data + inode->size, 0, new_cap - inode->size);
        inode->data = new_data;
        inode->capacity = new_cap;
    }

    memcpy(inode->data + vf->offset, buf, count);
    vf->offset += count;
    if (vf->offset > inode->size) inode->size = vf->offset;
    return (int)count;
}

static long ramfs_flseek(vfile_t *vf, long offset, int whence) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)inode->size + offset; break;
        default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    vf->offset = (size_t)new_off;
    return new_off;
}

static int ramfs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    inode_t *inode = (inode_t *)vf->vnode->fs_data;
    if (inode->type != FT_DIRECTORY) return -ENOTDIR;

    dir_entry_t *entries = (dir_entry_t *)inode->data;
    int n_entries = inode->size / sizeof(dir_entry_t);
    char *out = (char *)dirp;
    size_t total = 0;

    int idx = (int)(vf->offset / sizeof(dir_entry_t));
    while (idx < n_entries) {
        dir_entry_t *de = &entries[idx];
        if (de->name[0] != '\0') {
            size_t namelen = strlen(de->name);
            size_t reclen  = (offsetof(linux_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) break;

            linux_dirent64_t *d = (linux_dirent64_t *)(out + total);
            d->d_ino    = (uint64_t)de->inum;
            d->d_off    = (int64_t)(total + reclen);
            d->d_reclen = (uint16_t)reclen;
            d->d_type   = DT_UNKNOWN;
            inode_t *child = fs_find_inode_by_inum(de->inum);
            if (child) {
                if (child->type == FT_DIRECTORY) d->d_type = DT_DIR;
                else if (child->type == FT_REGULAR) d->d_type = DT_REG;
            }
            memcpy(d->d_name, de->name, namelen + 1);
            total += reclen;
        }
        idx++;
        vf->offset += sizeof(dir_entry_t);
    }
    return (int)total;
}

static int ramfs_fclose(vfile_t *vf) {
    (void)vf;
    return 0;
}

static vfile_ops_t g_ramfs_fops = {
    .read    = ramfs_fread,
    .write   = ramfs_fwrite,
    .lseek   = ramfs_flseek,
    .readdir = ramfs_freaddir,
    .close   = ramfs_fclose,
};

/* ============================================================
 * Mount resolution — find which mount owns a path
 * ============================================================ */

static mount_t *vfs_find_mount(const char *path) {
    mount_t *best = NULL;
    size_t   best_len = 0;
    for (int i = 0; i < g_nmounts; i++) {
        size_t len = strlen(g_mounts[i].path);
        if (strncmp(path, g_mounts[i].path, len) == 0 && len > best_len) {
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
    if (!path || !*path) return root;

    vnode_t *cur = root;
    cur->ref_count++;

    char buf[MAX_PATH_LEN];
    strncpy(buf, path, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';

    char *p = buf;
    while (*p == '/') p++;

    while (*p) {
        char *sep = strchr(p, '/');
        if (sep) *sep = '\0';

        if (strcmp(p, ".") == 0) {
            /* stay */
        } else if (strcmp(p, "..") == 0) {
            if (cur->parent && cur->parent != cur) {
                vnode_t *parent = cur->parent;
                parent->ref_count++;
                cur->ref_count--;
                cur = parent;
            }
        } else {
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                cur->ref_count--;
                return NULL;
            }
            vnode_t *next = NULL;
            int r = cur->ops->lookup(cur, p, &next);
            cur->ref_count--;
            if (r < 0 || !next) return NULL;
            cur = next;
        }

        if (sep) p = sep + 1;
        else break;
    }
    return cur;
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

/* Forward declaration */
extern vfile_t *fat32_open_vnode(vnode_t *vn, int flags);
extern vfile_t *ext4_open_vnode(vnode_t *vn, int flags);

static vfile_t *ramfs_open_vnode(vnode_t *vn, int flags) {
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return NULL;
    memset(vf, 0, sizeof(*vf));
    vf->vnode     = vn;
    vf->flags     = flags;
    vf->offset    = (flags & O_APPEND) ? vn->size : 0;
    vf->ref_count = 1;
    vf->ops       = &g_ramfs_fops;
    return vf;
}

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

    if (strcmp(resolved, "/dev/null") == 0) {
        vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
        if (!vf) return -ENOMEM;
        memset(vf, 0, sizeof(*vf));
        vf->ops = &g_null_ops; vf->ref_count = 1;
        int fd = vfs_alloc_fd(vf);
        if (fd < 0) { kfree(vf); return -EMFILE; }
        return fd;
    }
    if (strcmp(resolved, "/dev/zero") == 0) {
        vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
        if (!vf) return -ENOMEM;
        memset(vf, 0, sizeof(*vf));
        vf->ops = &g_zero_ops; vf->ref_count = 1;
        int fd = vfs_alloc_fd(vf);
        if (fd < 0) { kfree(vf); return -EMFILE; }
        return fd;
    }
    if (strcmp(resolved, "/dev/tty") == 0) {
        vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
        if (!vf) return -ENOMEM;
        memset(vf, 0, sizeof(*vf));
        /* Map it directly to UART (stdin/stdout) for now */
        vf->ops = &g_stdin_ops; vf->ref_count = 1;
        int fd = vfs_alloc_fd(vf);
        if (fd < 0) { kfree(vf); return -EMFILE; }
        return fd;
    }

    /* Find mount point */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) { kdebug("[VFS] open '%s': no mount\n", resolved); return -ENOENT; }

    const char *rel = strip_mount_prefix(resolved, mnt);
    vnode_t *vn = vnode_lookup_path(mnt->root, rel);

    if (!vn) {
        if (!(flags & O_CREAT)) { kdebug("[VFS] open '%s' (rel='%s'): not found, no O_CREAT\n", resolved, rel); return -ENOENT; }
        if (!mnt->root || !mnt->root->ops || !mnt->root->ops->create) { kdebug("[VFS] open '%s': root has no create ops\n", resolved); return -ENOSYS; }

        char parent_path[MAX_PATH_LEN];
        strncpy(parent_path, rel, MAX_PATH_LEN - 1);
        char *slash = strrchr(parent_path, '/');
        const char *fname = slash ? slash + 1 : rel;
        if (slash) *slash = '\0';
        else parent_path[0] = '\0';

        vnode_t *parent = vnode_lookup_path(mnt->root, parent_path);
        if (!parent || parent->type != VFS_FT_DIR) { kdebug("[VFS] open '%s': parent '%s' not found\n", resolved, parent_path); return -ENOENT; }
        if (!parent->ops || !parent->ops->create) { kdebug("[VFS] open '%s': parent has no create\n", resolved); return -ENOSYS; }

        int r = parent->ops->create(parent, fname, mode, &vn);
        if (r < 0) { kdebug("[VFS] open '%s': create failed r=%d\n", resolved, r); return r; }
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
    }

    if (!vf) return -ENOMEM;

    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        kfree(vf);
        return -EMFILE;
    }
    return gfd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= GFILE_MAX) return -EBADF;

    vfile_t *vf = g_files[fd];
    if (!vf) return -EBADF;

    vf->ref_count--;
    if (vf->ref_count <= 0) {
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        kfree(vf);
        g_files[fd] = NULL;
    }
    return 0;
}

/* ============================================================
 * VFS read / write / lseek  
 * ============================================================ */

int vfs_read(int fd, char *buf, size_t count) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->ops && vf->ops->read) return vf->ops->read(vf, buf, count);
        return -EBADF;
    }
    return -EBADF;
}

int vfs_write(int fd, const char *buf, size_t count) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->ops && vf->ops->write) return vf->ops->write(vf, buf, count);
        return -EBADF;
    }
    return -EBADF;
}

long vfs_lseek(int fd, long offset, int whence) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
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

    if (fd >= 0 && fd <= 2 && arg) {
        if (req == TCGETS) {
            memset(arg, 0, 36);
            return 0;
        }
        if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
            return 0;
        }
        if (req == TIOCGWINSZ) {
            memset(arg, 0, 8);
            return 0;
        }
    }

    if (vf->ops && vf->ops->ioctl) return vf->ops->ioctl(vf, req, arg);
    return -ENOTTY;
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
    if (!parent || parent->type != VFS_FT_DIR) return -ENOENT;
    if (!parent->ops || !parent->ops->mkdir) return -ENOTDIR;
    return parent->ops->mkdir(parent, name, mode);
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
    if (!parent) return -ENOENT;
    if (!parent->ops || !parent->ops->unlink) return -ENOTDIR;
    return parent->ops->unlink(parent, name);
}

int vfs_rename(const char *old, const char *newpath) {
    (void)old; (void)newpath;
    return -ENOSYS; /* TODO: unified rename */
}

int vfs_stat(const char *path, kstat_t *st) {
    if (strcmp(path, "/dev/null") == 0 || strcmp(path, "/dev/zero") == 0 ||
        strcmp(path, "/dev/tty") == 0) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFCHR | 0666;
        st->st_nlink = 1;
        st->st_blksize = 4096;
        return 0;
    }
    vnode_t *vn = vfs_resolve(path);
    if (!vn) return -ENOENT;
    if (vn->ops && vn->ops->stat) {
        int r = vn->ops->stat(vn, st);
        vn->ref_count--;
        return r;
    }
    vn->ref_count--;
    return -ENOSYS;
}

int vfs_fstat(int fd, kstat_t *st) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->vnode && vf->vnode->ops && vf->vnode->ops->stat)
            return vf->vnode->ops->stat(vf->vnode, st);
        if (fd >= 0 && fd <= 2) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFCHR | 0666;
            st->st_nlink = 1;
            st->st_blksize = 4096;
            return 0;
        }
    }
    return -EBADF;
}

int vfs_fstatat(int dirfd, const char *path, kstat_t *st, int flags) {
    (void)dirfd; (void)flags;
    return vfs_stat(path, st);
}

int vfs_faccessat(int dirfd, const char *path, int mode) {
    (void)dirfd; (void)mode;
    vnode_t *vn = vfs_resolve(path);
    if (!vn) {
        stat_t rfs;
        if (fs_stat(path, &rfs) < 0) return -ENOENT;
        return 0;
    }
    vn->ref_count--;
    return 0;
}

int vfs_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    (void)dirfd; (void)path; (void)buf; (void)sz;
    return -EINVAL;
}

int vfs_link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -ENOSYS;
}

int vfs_symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    return -ENOSYS;
}

int vfs_chdir(const char *path) {
    task_t *cur = proc_current();
    if (!cur) return -EINVAL;
    /* Verify exists and is directory */
    vnode_t *vn = vfs_resolve(path);
    if (!vn) {
        stat_t rfs;
        if (fs_stat(path, &rfs) < 0) return -ENOENT;
        if (rfs.st_type != FT_DIRECTORY) return -ENOTDIR;
        return fs_chdir(path);
    }
    if (vn->type != VFS_FT_DIR) { vn->ref_count--; return -ENOTDIR; }
    vn->ref_count--;
    return fs_chdir(path);
}

int vfs_getcwd(char *buf, size_t size) {
    return fs_getcwd(buf, size);
}

/* ============================================================
 * Pipe
 * ============================================================ */

#define PIPE_BUF_SIZE   4096

typedef struct pipe_buf {
    char     data[PIPE_BUF_SIZE];
    size_t   head, tail, used;
    int      writer_closed;
    int      reader_closed;
    int      ref;
} pipe_buf_t;

static int pipe_read(vfile_t *vf, char *buf, size_t count) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    while (pb->used == 0) {
        if (pb->writer_closed) return 0; /* EOF */
        proc_yield();
    }
    size_t n = pb->used < count ? pb->used : count;
    for (size_t i = 0; i < n; i++) {
        buf[i] = pb->data[pb->tail];
        pb->tail = (pb->tail + 1) % PIPE_BUF_SIZE;
        pb->used--;
    }
    return (int)n;
}

static int pipe_write(vfile_t *vf, const char *buf, size_t count) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (pb->reader_closed) return -EPIPE;
    size_t n = 0;
    while (n < count) {
        while (pb->used == PIPE_BUF_SIZE) proc_yield();
        pb->data[pb->head] = buf[n++];
        pb->head = (pb->head + 1) % PIPE_BUF_SIZE;
        pb->used++;
    }
    return (int)n;
}

static int pipe_read_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) { pb->reader_closed = 1; pb->ref--; if (!pb->ref) kfree(pb); }
    kfree(vf);
    return 0;
}

static int pipe_write_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) { pb->writer_closed = 1; pb->ref--; if (!pb->ref) kfree(pb); }
    kfree(vf);
    return 0;
}

static vfile_ops_t g_pipe_read_ops  = { .read = pipe_read,  .write = devfs_null_write, .close = pipe_read_close  };
static vfile_ops_t g_pipe_write_ops = { .read = devfs_null_read, .write = pipe_write, .close = pipe_write_close };

int vfs_pipe(int pipefd[2]) {
    pipe_buf_t *pb = (pipe_buf_t *)kmalloc(sizeof(pipe_buf_t));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(*pb));
    pb->ref = 2;

    vfile_t *rd = (vfile_t *)kmalloc(sizeof(vfile_t));
    vfile_t *wr = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!rd || !wr) { kfree(pb); if (rd) kfree(rd); if (wr) kfree(wr); return -ENOMEM; }

    memset(rd, 0, sizeof(*rd)); rd->ops = &g_pipe_read_ops;  rd->priv = pb; rd->ref_count = 1;
    memset(wr, 0, sizeof(*wr)); wr->ops = &g_pipe_write_ops; wr->priv = pb; wr->ref_count = 1;

    int fdrd = vfs_alloc_fd(rd);
    int fdwr = vfs_alloc_fd(wr);
    if (fdrd < 0 || fdwr < 0) {
        if (fdrd >= 0) vfs_free_gfd(fdrd);
        kfree(rd); kfree(wr); kfree(pb);
        return -EMFILE;
    }
    pipefd[0] = fdrd;
    pipefd[1] = fdwr;
    return 0;
}

/* ============================================================
 * dup / dup3 / fcntl
 * ============================================================ */

int vfs_dup(int fd) {
    if (fd == 0 || fd == 1 || fd == 2) {
        /* Find a new slot and alias it */
        for (int i = 3; i < GFILE_MAX; i++) {
            if (!g_files[i]) { g_files[i] = fd == 0 ? &g_stdin_file :
                               fd == 1 ? &g_stdout_file : &g_stderr_file; return i; }
        }
        return -EMFILE;
    }
    if (fd < 0 || fd >= GFILE_MAX || !g_files[fd]) return -EBADF;
    vfile_t *vf = g_files[fd];
    vf->ref_count++;
    for (int i = 3; i < GFILE_MAX; i++) {
        if (!g_files[i]) { g_files[i] = vf; return i; }
    }
    vf->ref_count--;
    return -EMFILE;
}

int vfs_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    if (oldfd == newfd) return newfd;
    if (newfd >= GFILE_MAX || newfd < 0) return -EBADF;
    if (g_files[newfd]) vfs_close(newfd);
    if (oldfd >= 0 && oldfd < GFILE_MAX && g_files[oldfd]) {
        g_files[newfd] = g_files[oldfd];
        g_files[newfd]->ref_count++;
        return newfd;
    }
    return -EBADF;
}

int vfs_fcntl(int fd, int cmd, long arg) {
    /* F_GETFL=3, F_SETFL=4, F_DUPFD=0, F_GETFD=1, F_SETFD=2 */
    if (cmd == 3) { /* F_GETFL */
        if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) return g_files[fd]->flags;
        return 0;
    }
    if (cmd == 4) { /* F_SETFL */
        if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) g_files[fd]->flags = (int)arg;
        return 0;
    }
    if (cmd == 0) { /* F_DUPFD */
        return vfs_dup(fd);
    }
    if (cmd == 1) return 0; /* F_GETFD */
    if (cmd == 2) return 0; /* F_SETFD */
    return -EINVAL;
}

/* ============================================================
 * VFS Mount
 * ============================================================ */

int vfs_mount(const char *dev, const char *path, const char *fstype, int flags) {
    (void)dev; (void)path; (void)fstype; (void)flags;
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
        mnt->fs_data = NULL;

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
        mnt->fs_data = NULL;

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
    vn->ref_count--;
    return r;
}

int vfs_ftruncate(int fd, size_t size) {
    (void)fd; (void)size;
    return 0; /* stub */
}

/* ============================================================
 * VFS init — set up std streams, root ramfs mount
 * ============================================================ */

void vfs_init(void) {
    fs_init();
    memset(g_files, 0, sizeof(g_files));
    memset(g_mounts, 0, sizeof(g_mounts));
    g_nmounts = 0;

    /* Install std streams at global fds 0,1,2 */
    g_files[STDIN_FILENO]  = &g_stdin_file;
    g_files[STDOUT_FILENO] = &g_stdout_file;
    g_files[STDERR_FILENO] = &g_stderr_file;

    /* Register ramfs as root "/" mount */
    mount_t *mnt = &g_mounts[g_nmounts++];
    memset(mnt, 0, sizeof(*mnt));
    strcpy(mnt->path, "/");
    mnt->type = FS_TYPE_RAMFS;
    mnt->root = ramfs_make_vnode(mnt, fs_get_root());

    printf("[VFS] Initialized (root=ramfs)\n");

    /* Mount procfs at /proc */
    {
        extern vnode_t *procfs_mount(void);
        extern vfile_t *procfs_open_vnode(vnode_t *vn, int flags);
        fs_mkdir("/proc");
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
