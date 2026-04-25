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
#include "fs/fs.h"
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
#include "drv/virtio_blk.h"
#include "fs/block_cache.h"

/* ============================================================
 * Global open-file table
 * ============================================================ */
#define GFILE_MAX   VFS_MAX_OPEN

static vfile_t *g_files[GFILE_MAX];  // 全局文件表

/* Mount table (simple linear) */
#define MAX_MOUNTS  8
static mount_t g_mounts[MAX_MOUNTS];  // 挂载表
static int     g_nmounts = 0;  // 已挂载数量

/* ---- File descriptor allocation ---- */

// 分配全局文件描述符
int vfs_alloc_fd(vfile_t *vf) {
    /* Find slot in global file table */
    int gfd = -1;
    for (int i = 3; i < GFILE_MAX; i++) { /* 0,1,2 reserved for std??? */
        if (!g_files[i]) { g_files[i] = vf; gfd = i; break; }
    }
    return gfd;
}

// 释放全局文件描述符
static void vfs_free_gfd(int gfd) {
    if (gfd >= 0 && gfd < GFILE_MAX) g_files[gfd] = NULL;
}

// 获取文件描述符对应的 vfile
vfile_t *vfs_get_file(int fd) {
    if (fd < 0 || fd >= GFILE_MAX) return NULL;
    return g_files[fd];
}

/* ============================================================
 * Per-process fd table
 * We map per-process fds → global gfds
 * For simplicity, the per-process fd IS the global gfd.
 * ============================================================ */

// 初始化进程的文件描述符表
void vfs_proc_init_fds(int *fd_table) {
    for (int i = 0; i < MAX_FILES; i++) fd_table[i] = -1;
    fd_table[0] = 0; if (g_files[0]) g_files[0]->ref_count++;
    fd_table[1] = 1; if (g_files[1]) g_files[1]->ref_count++;
    fd_table[2] = 2; if (g_files[2]) g_files[2]->ref_count++;
}

// 初始化进程的标准 I/O 文件描述符（如果未设置）
void vfs_proc_init_stdio_defaults(int *fd_table) {
    if (fd_table[0] < 0) { fd_table[0] = 0; if (g_files[0]) g_files[0]->ref_count++; }
    if (fd_table[1] < 0) { fd_table[1] = 1; if (g_files[1]) g_files[1]->ref_count++; }
    if (fd_table[2] < 0) { fd_table[2] = 2; if (g_files[2]) g_files[2]->ref_count++; }
}

// 复制进程文件描述符表（用于 fork）
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
        if (fd_table[i] >= 0) {
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
    if (c == '\r') c = '\n';
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
static vfile_ops_t g_pipe_read_ops;
static vfile_ops_t g_pipe_write_ops;

static vfile_t g_stdin_file  = { .ref_count = 999, .ops = &g_stdin_ops,  .flags = O_RDONLY };
static vfile_t g_stdout_file = { .ref_count = 999, .ops = &g_stdout_ops, .flags = O_WRONLY };
static vfile_t g_stderr_file = { .ref_count = 999, .ops = &g_stderr_ops, .flags = O_WRONLY };

/* Check if a vfile is one of the special stdin/stdout/stderr char devices */
static int is_special_tty(vfile_t *vf) {
    if (!vf) return 0;
    return (vf == &g_stdin_file || vf == &g_stdout_file || vf == &g_stderr_file ||
            vf->ops == &g_stdin_ops || vf->ops == &g_stdout_ops || vf->ops == &g_stderr_ops);
}

static int is_pipe_vfile(vfile_t *vf) {
    return vf && (vf->ops == &g_pipe_read_ops || vf->ops == &g_pipe_write_ops);
}

static int is_char_device_vfile(vfile_t *vf) {
    return vf && (is_special_tty(vf) || vf->ops == &g_null_ops || vf->ops == &g_zero_ops);
}

static void fill_char_kstat(kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

static void fill_pipe_kstat(kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} ktermios_compat_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} kwinsize_t;

static void fill_default_termios(ktermios_compat_t *tio) {
    memset(tio, 0, sizeof(*tio));
    tio->c_iflag = 0x500;      /* ICRNL | IXON */
    tio->c_oflag = 0x5;        /* OPOST | ONLCR */
    tio->c_cflag = 0xB0;       /* CREAD | CS8 | B38400 */
    tio->c_lflag = 0x8a3b;     /* ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN | ECHOCTL | ECHOKE */
    tio->c_cc[0] = 3;          /* VINTR  */
    tio->c_cc[1] = 28;         /* VQUIT  */
    tio->c_cc[2] = 127;        /* VERASE */
    tio->c_cc[3] = 21;         /* VKILL  */
    tio->c_cc[4] = 4;          /* VEOF   */
    tio->c_cc[5] = 0;          /* VTIME  */
    tio->c_cc[6] = 1;          /* VMIN   */
    tio->c_cc[8] = 17;         /* VSTART */
    tio->c_cc[9] = 19;         /* VSTOP  */
    tio->c_cc[10] = 26;        /* VSUSP  */
    tio->c_cc[12] = 18;        /* VREPRINT */
    tio->c_cc[13] = 15;        /* VDISCARD */
    tio->c_cc[14] = 23;        /* VWERASE */
    tio->c_cc[15] = 22;        /* VLNEXT */
    tio->c_ispeed = 15;        /* B38400 */
    tio->c_ospeed = 15;        /* B38400 */
}

static void fill_default_winsize(kwinsize_t *ws) {
    ws->ws_row = 24;
    ws->ws_col = 80;
    ws->ws_xpixel = 0;
    ws->ws_ypixel = 0;
}

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
            if (cur->type != VFS_FT_DIR || !cur->ops || !cur->ops->lookup) {
                cur->ref_count--;
                return NULL;
            }
            vnode_t *next = NULL;
            int r = cur->ops->lookup(cur, p, &next);
            if (r < 0 || !next) {
                cur->ref_count--;
                return NULL;
            }
            vnode_t *parent = cur;
            cur = next;

            if (cur->type == VFS_FT_SYMLINK) {
                if (++symlink_depth > 8) {
                    parent->ref_count--;
                    cur->ref_count--;
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
            return -ENOENT;
        }
        if (!parent->ops || !parent->ops->create) {
            kdebug("[VFS] open '%s': parent has no create\n", resolved);
            vnode_put(parent);
            return -ENOSYS;
        }

        int r = parent->ops->create(parent, fname, mode, &vn);
        vnode_put(parent);
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
    } else if (mnt->type == FS_TYPE_PROCFS) {
        extern vfile_t *procfs_open_vnode(vnode_t *vn, int flags);
        vf = procfs_open_vnode(vn, flags);
    }

    if (!vf) { vnode_put(vn); return -ENOMEM; }

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

    vfile_t *vf = g_files[fd];
    if (!vf) return -EBADF;

    vf->ref_count--;
    if (vf->ref_count <= 0) {
        vnode_t *vn = vf->vnode;
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        kfree(vf);
        g_files[fd] = NULL;
        vnode_put(vn);
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
        if (is_special_tty(vf) || is_pipe_vfile(vf)) return -ESPIPE;
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

    if (is_special_tty(vf) && arg) {
        if (req == TCGETS) {
            ktermios_compat_t tio;
            fill_default_termios(&tio);
            if (copy_to_user(arg, &tio, sizeof(tio)) < 0) return -EFAULT;
            return 0;
        }
        if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
            return 0;
        }
        if (req == TIOCGWINSZ) {
            kwinsize_t ws;
            fill_default_winsize(&ws);
            if (copy_to_user(arg, &ws, sizeof(ws)) < 0) return -EFAULT;
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
    if (!parent || parent->type != VFS_FT_DIR) {
        vnode_put(parent);
        return -ENOENT;
    }
    if (!parent->ops || !parent->ops->mkdir) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    int r = parent->ops->mkdir(parent, name, mode);
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
    if (!parent) return -ENOENT;
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
        return -ENOENT;
    }
    if (old_dir->type != VFS_FT_DIR || new_dir->type != VFS_FT_DIR) {
        vnode_put(old_dir);
        vnode_put(new_dir);
        return -ENOTDIR;
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
        return -ENOENT;
    }
    if (!parent->ops || !parent->ops->rmdir) {
        vnode_put(parent);
        return -ENOSYS;
    }
    int r = parent->ops->rmdir(parent, name);
    vnode_put(parent);
    return r;
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
        vnode_put(vn);
        return r;
    }
    vnode_put(vn);
    return -ENOSYS;
}

int vfs_fstat(int fd, kstat_t *st) {
    if (fd >= 0 && fd < GFILE_MAX && g_files[fd]) {
        vfile_t *vf = g_files[fd];
        if (vf->vnode && vf->vnode->ops && vf->vnode->ops->stat)
            return vf->vnode->ops->stat(vf->vnode, st);
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
    (void)oldpath; (void)newpath;
    return -ENOSYS;
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
    /* Verify exists and is directory */
    vnode_t *vn = vfs_resolve(path);
    if (!vn) {
        stat_t rfs;
        if (fs_stat(path, &rfs) < 0) return -ENOENT;
        if (rfs.st_type != FT_DIRECTORY) return -ENOTDIR;
        return fs_chdir(path);
    }
    if (vn->type != VFS_FT_DIR) { vnode_put(vn); return -ENOTDIR; }
    vnode_put(vn);
    return fs_chdir(path);
}

int vfs_getcwd(char *buf, size_t size) {
    return fs_getcwd(buf, size);
}

/* ============================================================
 * Pipe
 * ============================================================ */

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
        while (pb->used == PIPE_BUF_SIZE) {
            if (pb->reader_closed) return n ? (int)n : -EPIPE;
            proc_yield();
        }
        pb->data[pb->head] = buf[n++];
        pb->head = (pb->head + 1) % PIPE_BUF_SIZE;
        pb->used++;
    }
    return (int)n;
}

static int pipe_read_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) { pb->reader_closed = 1; pb->ref--; if (!pb->ref) kfree(pb); }
    return 0;
}

static int pipe_write_close(vfile_t *vf) {
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) { pb->writer_closed = 1; pb->ref--; if (!pb->ref) kfree(pb); }
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
        if (fdrd >= 0) { vfs_close(fdrd); vfs_free_gfd(fdrd); }
        if (fdwr >= 0) { vfs_close(fdwr); vfs_free_gfd(fdwr); }
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

static int vfs_dupfd(int fd, int minfd) {
    if (minfd < 0) minfd = 0;
    if (fd == 0 || fd == 1 || fd == 2) {
        for (int i = minfd; i < GFILE_MAX; i++) {
            if (!g_files[i]) {
                g_files[i] = (fd == 0) ? &g_stdin_file :
                             (fd == 1) ? &g_stdout_file : &g_stderr_file;
                return i;
            }
        }
        return -EMFILE;
    }
    if (fd < 0 || fd >= GFILE_MAX || !g_files[fd]) return -EBADF;
    vfile_t *vf = g_files[fd];
    vf->ref_count++;
    for (int i = minfd; i < GFILE_MAX; i++) {
        if (!g_files[i]) { g_files[i] = vf; return i; }
    }
    vf->ref_count--;
    return -EMFILE;
}

int vfs_dup(int fd) {
    return vfs_dupfd(fd, 3);
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
        return vfs_dupfd(fd, (int)arg);
    }
    if (cmd == 1030) { /* F_DUPFD_CLOEXEC */
        return vfs_dupfd(fd, (int)arg);
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
    vnode_put(vn);
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
    mnt->root = ramfs_mount(mnt);

    printf("[VFS] Initialized (root=ramfs)\n");

    vfs_mkdir("/tmp", 0755);

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
