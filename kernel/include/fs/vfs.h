#ifndef _VFS_H
#define _VFS_H

#include "core/types.h"
#include "core/consts.h"

/* ============================================================
 * VFS — Virtual Filesystem Switch
 * Inspired by Linux VFS and RocketOS fs design.
 * Provides a unified interface over ramfs and FAT32 mounts.
 * ============================================================ */

/* File types */
#define VFS_FT_UNKNOWN   0
#define VFS_FT_REGULAR   1
#define VFS_FT_DIR       2
#define VFS_FT_SYMLINK   3

/* Filesystem type IDs */
#define FS_TYPE_RAMFS    1
#define FS_TYPE_FAT32    2
#define FS_TYPE_EXT4     3
#define FS_TYPE_PROCFS   4
#define FS_TYPE_DEVFS    5

/* ---- Forward declarations ---- */
struct vnode;
struct vfile;
struct vfs_ops;
struct vnode_ops;
struct mount;

struct bcache;
struct block_dev;

/* ---- stat structure (Linux compatible) ---- */
typedef struct kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
} kstat_t;

/* ---- Mode bits ---- */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFLNK  0120000
#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

/* ---- Vnode operations ---- */
typedef struct vnode_ops {
    int     (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int     (*create)(struct vnode *dir, const char *name, int mode, struct vnode **out);
    int     (*mkdir)(struct vnode *dir, const char *name, int mode);
    int     (*unlink)(struct vnode *dir, const char *name);
    int     (*rmdir)(struct vnode *dir, const char *name);
    int     (*rename)(struct vnode *old_dir, const char *old_name,
                      struct vnode *new_dir, const char *new_name);
    int     (*link)(struct vnode *dir, const char *name, struct vnode *target);
    int     (*symlink)(struct vnode *dir, const char *name, const char *target);
    int     (*readlink)(struct vnode *vn, char *buf, size_t sz);
    int     (*stat)(struct vnode *vn, kstat_t *st);
    int     (*truncate)(struct vnode *vn, size_t size);
    void    (*release)(struct vnode *vn);
} vnode_ops_t;

/* ---- File operations ---- */
typedef struct vfile_ops {
    int     (*read)(struct vfile *vf, char *buf, size_t count);
    int     (*write)(struct vfile *vf, const char *buf, size_t count);
    long    (*lseek)(struct vfile *vf, long offset, int whence);
    int     (*readdir)(struct vfile *vf, void *dirp, size_t count);
    int     (*ioctl)(struct vfile *vf, unsigned long req, void *arg);
    int     (*close)(struct vfile *vf);
} vfile_ops_t;

/* ---- Vnode (in-memory inode) ---- */

typedef struct vnode {
    uint64_t        ino;            /* inode number */
    int             type;           /* VFS_FT_* */
    uint32_t        mode;           /* permissions */
    size_t          size;
    int             ref_count;
    struct vnode   *parent;
    struct mount   *mnt;            /* which mount owns this */
    void           *fs_data;        /* fs-private data */
    vnode_ops_t    *ops;
} vnode_t;

/* ---- Open file ---- */
typedef struct vfile {
    vnode_t        *vnode;
    int             flags;
    size_t          offset;
    int             ref_count;
    vfile_ops_t    *ops;
    void           *priv;           /* fs/pipe private data */
} vfile_t;

/* ---- Mount point ---- */
typedef struct mount {
    int             type;           /* FS_TYPE_* */
    char            path[MAX_PATH_LEN];
    vnode_t        *root;           /* root vnode of this mount */
    void           *fs_data;
} mount_t;

/* ---- Open file table (global) ---- */
#define VFS_MAX_OPEN   8192

/* ---- a20_dirent64 (for getdents64 syscall) ---- */
typedef struct a20_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];           /* variable length */
} __attribute__((packed)) a20_dirent64_t;

/* ============================================================
 * VFS API
 * ============================================================ */

void     vfs_init(void);

/* Path resolution */
void     vnode_put(vnode_t *vn);
vnode_t *vfs_resolve(const char *path);
vnode_t *vfs_resolve_at(const char *path, const char *cwd);

/* File operations */
int      vfs_open(const char *path, int flags, int mode);
int      vfs_close(int fd);
int      vfs_read(int fd, char *buf, size_t count);
int      vfs_write(int fd, const char *buf, size_t count);
long     vfs_lseek(int fd, long offset, int whence);
int      vfs_getdents64(int fd, void *dirp, size_t count);
int      vfs_ioctl(int fd, unsigned long req, void *arg);

/* Directory operations */
int      vfs_mkdir(const char *path, int mode);
int      vfs_unlink(const char *path);
int      vfs_rmdir(const char *path);
int      vfs_rename(const char *old, const char *newpath);
int      vfs_stat(const char *path, kstat_t *st);
int      vfs_fstat(int fd, kstat_t *st);
int      vfs_fstatat(int dirfd, const char *path, kstat_t *st, int flags);
int      vfs_faccessat(int dirfd, const char *path, int mode);
int      vfs_readlinkat(int dirfd, const char *path, char *buf, size_t sz);
int      vfs_link(const char *oldpath, const char *newpath);
int      vfs_symlink(const char *target, const char *linkpath);

/* Working directory */
int      vfs_chdir(const char *path);
int      vfs_getcwd(char *buf, size_t size);

/* Mount */
int      vfs_mount(const char *dev, const char *path, const char *fstype, int flags);
int      vfs_mount_bc(const char *path, const char *fstype, struct bcache *bc);
int      vfs_umount(const char *path);

/* Pipe */
int      vfs_pipe(int pipefd[2]);

/* file table access (for dup/dup3) */
vfile_t *vfs_get_file(int fd);
int      vfs_alloc_fd(vfile_t *vf);
int      vfs_dup(int fd);
int      vfs_dup3(int oldfd, int newfd, int flags);
int      vfs_fcntl(int fd, int cmd, long arg);

/* Truncate */
int      vfs_truncate(const char *path, size_t size);
int      vfs_ftruncate(int fd, size_t size);

/* Per-process fd table management */
void     vfs_proc_init_fds(int *fd_table);
void     vfs_proc_init_stdio_defaults(int *fd_table);
void     vfs_proc_copy_fds(const int *src, int *dst);
void     vfs_proc_close_all_fds(int *fd_table);

#endif /* _VFS_H */
