#ifndef _VFS_H
#define _VFS_H

#include "core/types.h"
#include "core/consts.h"
#include "core/refcount.h"

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
#define FS_TYPE_CGROUP   6

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
#define S_ISUID  04000
#define S_ISGID  02000
#define S_ISVTX  01000

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
    int     (*writepage)(struct vnode *vn, uint64_t index,
                         const void *data, size_t len);
    int     (*chmod)(struct vnode *vn, int mode);
    int     (*chown)(struct vnode *vn, int uid, int gid);
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

/*
 * vnode lifetime:
 * - A vnode reference keeps the in-memory inode alive.
 * - Filesystems return referenced vnodes to VFS callers.
 * - dcache entries hold references and must release them on invalidation.
 * - Direct ref_count edits are legacy; new code should use vnode_put() and a
 *   future vnode_get()/refcount_t wrapper.
 */
typedef struct vnode {
    uint64_t        ino;            /* inode number */
    int             type;           /* VFS_FT_* */
    uint32_t        mode;           /* permissions */
    uint32_t        uid;
    uint32_t        gid;
    size_t          size;
    refcount_t      ref_count;
    struct vnode   *parent;
    struct mount   *mnt;            /* which mount owns this */
    void           *fs_data;        /* fs-private data */
    vnode_ops_t    *ops;
} vnode_t;

/*
 * vfile lifetime:
 * - vfile objects represent global open-file entries; process fds point to
 *   them through fdtable mappings.
 * - ref_count accounts for fdtable references and temporary kernel users.
 * - Direct ref_count edits are legacy; new code should move toward
 *   file_get()/file_put() style APIs.
 */
typedef struct vfile {
    vnode_t        *vnode;
    int             flags;
    size_t          offset;
    refcount_t      ref_count;
    int             owner_type;
    int             owner_pid;
    int             owner_signal;
    int             seals;
    uint64_t        rw_hint;
    char            path[MAX_PATH_LEN];
    vfile_ops_t    *ops;
    void           *priv;           /* fs/pipe private data */
} vfile_t;

/* ---- Mount point ---- */
typedef struct mount {
    int             type;           /* FS_TYPE_* */
    int             flags;
    char            path[MAX_PATH_LEN];
    char            dev[64];        /* device/source name */
    char            fstype[32];     /* filesystem type string */
    char            opts[256];      /* mount options */
    vnode_t        *root;           /* root vnode of this mount */
    void           *fs_data;
} mount_t;

/* ---- Open file table (global) ---- */
#define VFS_MAX_OPEN   8192

/* ---- Kernel directory entry wire format used by VFS readdir callbacks. ---- */
typedef struct vfs_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1];           /* variable length */
} __attribute__((packed)) vfs_dirent64_t;

/* ============================================================
 * VFS API
 * ============================================================ */

void     vfs_init(void);

/* Path resolution */
void     vnode_ref_init(vnode_t *vn, int refs);
void     vnode_get(vnode_t *vn);
int      vnode_ref_read(vnode_t *vn);
void     vnode_put(vnode_t *vn);
vnode_t *vfs_resolve(const char *path);
vnode_t *vfs_resolve_at(const char *path, const char *cwd);

/* File operations */
int      vfs_open(const char *path, int flags, int mode);
int      vfs_close(int fd);
int      vfs_read(int fd, char *buf, size_t count);
int      vfs_write(int fd, const char *buf, size_t count);
int      vfs_read_file(vfile_t *vf, char *buf, size_t count);
int      vfs_write_file(vfile_t *vf, const char *buf, size_t count);
int      vfs_pread(int fd, char *buf, size_t count, uint64_t offset);
long     vfs_lseek(int fd, long offset, int whence);
int      vfs_getdents64(int fd, void *dirp, size_t count);
int      vfs_ioctl(int fd, unsigned long req, void *arg);
int      vfs_sync(void);
int      vfs_fsync(int fd);
int      vfs_poll_events(int fd, short events);

/* Directory operations */
int      vfs_mkdir(const char *path, int mode);
int      vfs_unlink(const char *path);
int      vfs_rmdir(const char *path);
int      vfs_rename(const char *old, const char *newpath);
int      vfs_stat(const char *path, kstat_t *st);
int      vfs_fstat(int fd, kstat_t *st);
int      vfs_fstatat(int dirfd, const char *path, kstat_t *st, int flags);
int      vfs_faccessat(int dirfd, const char *path, int mode);
int      vfs_faccessat2(int dirfd, const char *path, int mode, int flags);
int      vfs_chmodat(int dirfd, const char *path, int mode, int flags);
int      vfs_fchmod(int fd, int mode);
int      vfs_chownat(int dirfd, const char *path, int uid, int gid, int flags);
int      vfs_fchown(int fd, int uid, int gid);
int      vfs_utimensat(int dirfd, const char *path, const uint64_t times[4], int flags);
int      vfs_futimens(int fd, const uint64_t times[4]);
int      vfs_readlinkat(int dirfd, const char *path, char *buf, size_t sz);
int      vfs_link(const char *oldpath, const char *newpath);
int      vfs_symlink(const char *target, const char *linkpath);

/* Working directory */
int      vfs_chdir(const char *path);
int      vfs_getcwd(char *buf, size_t size);

/* Mount */
int      vfs_mount(const char *dev, const char *path, const char *fstype, int flags, const char *data);
int      vfs_mount_bc(const char *path, const char *fstype, struct bcache *bc);
int      vfs_umount(const char *path);

/* Pipe */
int      vfs_pipe(int pipefd[2]);

/* file table access (for dup/dup3) */
vfile_t *vfs_get_file(int fd);
vfile_t *vfs_get_file_ref(int fd);
void     vfs_put_file_ref(int fd, vfile_t *vf);
int      vfs_ref_fd(int fd);
int      vfs_alloc_fd(vfile_t *vf);
int      vfs_dup(int fd);
int      vfs_dup3(int oldfd, int newfd, int flags);
int      vfs_fcntl(int fd, int cmd, long arg);
int      vfs_flock(int fd, int operation);
void     vfs_release_process_locks(int pid);

/* Truncate */
int      vfs_truncate(const char *path, size_t size);
int      vfs_ftruncate(int fd, size_t size);

#endif /* _VFS_H */
