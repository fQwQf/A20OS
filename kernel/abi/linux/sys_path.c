#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

int64_t sys_mkdirat(int dirfd, const char *path, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    return vfs_mkdir(full, mode);
}

int64_t sys_unlinkat(int dirfd, const char *path, int flags) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    if (flags & ~(AT_REMOVEDIR)) return -EINVAL;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    if (flags & AT_REMOVEDIR) return vfs_rmdir(full);
    return vfs_unlink(full);
}

int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags) {
    if (flags) return -EINVAL;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    char fold[MAX_PATH_LEN], fnew[MAX_PATH_LEN];
    int pr = syscall_path_at(olddir, kold, fold, sizeof(fold));
    if (pr < 0) return pr;
    pr = syscall_path_at(newdir, knew, fnew, sizeof(fnew));
    if (pr < 0) return pr;
    return vfs_rename(fold, fnew);
}

int64_t sys_chdir(const char *path) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (copied < 0) return -EFAULT;
    if (copied >= MAX_PATH_LEN - 1 && kpath[MAX_PATH_LEN - 1] == '\0')
        return -ENAMETOOLONG;
    return vfs_chdir(kpath);
}

int64_t sys_fchdir(int fd) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) {
        vfs_put_file_ref((int)gfd, vf);
        return -EBADF;
    }
    int r = 0;
    char path[MAX_PATH_LEN];
    if (vf->vnode->type != VFS_FT_DIR) {
        r = -ENOTDIR;
    } else if (vf->path[0] == '\0') {
        r = -ENOENT;
    } else {
        strncpy(path, vf->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    vfs_put_file_ref((int)gfd, vf);
    return r < 0 ? r : vfs_chdir(path);
}

int64_t sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -EFAULT;
    size_t len = strlen(t->fs.cwd) + 1;
    if (size < len) return -ERANGE;
    if (copy_to_user(buf, t->fs.cwd, len) < 0) return -EFAULT;
    return (int64_t)len;
}

static uint32_t user_visible_mode(uint32_t mode) {
    return mode;
}

static int path_is_shebang_script(const char *path)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    char magic[2];
    int n = vfs_read(fd, magic, sizeof(magic));
    vfs_close(fd);
    return n >= 2 && magic[0] == '#' && magic[1] == '!';
}

static void kstat_apply_script_exec(const char *path, kstat_t *kst)
{
    if (!path || !kst)
        return;
    if ((kst->st_mode & S_IFMT) != S_IFREG)
        return;
    if (kst->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return;
    if (path_is_shebang_script(path))
        kst->st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
}

static void copy_kstat_to_user(void *st, const kstat_t *kst) {
    uint64_t buf64[128 / 8];
    memset(buf64, 0, sizeof(buf64));
    uint8_t *buf = (uint8_t *)buf64;
    uint64_t *u64 = (uint64_t *)buf;
    uint32_t *u32 = (uint32_t *)buf;
    u64[0]  = kst->st_dev;
    u64[1]  = kst->st_ino;
    u32[4]  = user_visible_mode(kst->st_mode);
    u32[5]  = kst->st_nlink;
    u32[6]  = kst->st_uid;
    u32[7]  = kst->st_gid;
    u64[4]  = kst->st_rdev;
    u64[5]  = 0;            /* __pad1 */
    u64[6]  = kst->st_size;
    u32[14] = kst->st_blksize;
    u32[15] = 0;            /* __pad2 */
    u64[8]  = kst->st_blocks;
    u64[9]  = kst->st_atime;
    u64[10] = kst->st_atime_nsec;
    u64[11] = kst->st_mtime;
    u64[12] = kst->st_mtime_nsec;
    u64[13] = kst->st_ctime;
    u64[14] = kst->st_ctime_nsec;
    u32[30] = 0;            /* __unused4 */
    u32[31] = 0;            /* __unused5 */
    copy_to_user(st, buf, sizeof(buf64));
}

int64_t sys_fstat(int fd, void *st) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    kstat_t kst;
    int r = vfs_fstat(gfd, &kst);
    if (r < 0) return r;
    if (!st) return -EFAULT;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags) {
    if (!path || !st) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r;
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = fdtable_get_current(dirfd);
        if (gfd < 0) return gfd;
        r = vfs_fstat((int)gfd, &kst);
    } else {
        char full[MAX_PATH_LEN];
        int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
        if (pr < 0) return pr;
        r = vfs_fstatat(AT_FDCWD, full, &kst, flags);
        if (r == 0)
            kstat_apply_script_exec(full, &kst);
    }
    if (r < 0) return r;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    if (sz > LINUX_IO_CHUNK_SIZE) sz = LINUX_IO_CHUNK_SIZE;
    char *kbuf = proc_scratch_buffer(LINUX_IO_CHUNK_SIZE);
    if (!kbuf) return -ENOMEM;
    int64_t r = vfs_readlinkat(AT_FDCWD, full, kbuf, sz);
    if (r > 0) {
        if (copy_to_user(buf, kbuf, (size_t)r) < 0) return -EFAULT;
    }
    return r;
}

int64_t sys_faccessat(int dirfd, const char *path, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (copied < 0) return -EFAULT;
    if (copied >= MAX_PATH_LEN - 1) return -ENAMETOOLONG;
    if (kpath[0] == '\0') return -ENOENT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    return vfs_faccessat(AT_FDCWD, full, mode);
}

int64_t sys_faccessat2(int dirfd, const char *path, int mode, int flags) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (copied < 0) return -EFAULT;
    if (copied >= MAX_PATH_LEN - 1) return -ENAMETOOLONG;
    if (kpath[0] == '\0' && !(flags & AT_EMPTY_PATH)) return -ENOENT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    return vfs_faccessat2(AT_FDCWD, full, mode, flags);
}

int64_t sys_chmod(const char *path, int mode) {
    return sys_fchmodat(AT_FDCWD, path, mode, 0);
}

int64_t sys_fchmod(int fd, int mode) {
    int64_t gfd = fdtable_get_current(fd);
    return gfd < 0 ? gfd : vfs_fchmod((int)gfd, mode);
}

int64_t sys_fchmodat(int dirfd, const char *path, int mode, int flags) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = fdtable_get_current(dirfd);
        if (gfd < 0) return gfd;
        return vfs_fchmod((int)gfd, mode);
    }
    if (kpath[0] == '\0') return -ENOENT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    return vfs_chmodat(AT_FDCWD, full, mode, flags);
}

int64_t sys_chown(const char *path, int uid, int gid) {
    return sys_fchownat(AT_FDCWD, path, uid, gid, 0);
}

int64_t sys_lchown(const char *path, int uid, int gid) {
    return sys_fchownat(AT_FDCWD, path, uid, gid, AT_SYMLINK_NOFOLLOW);
}

int64_t sys_fchown(int fd, int uid, int gid) {
    int64_t gfd = fdtable_get_current(fd);
    return gfd < 0 ? gfd : vfs_fchown((int)gfd, uid, gid);
}

int64_t sys_fchownat(int dirfd, const char *path, int uid, int gid, int flags) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (copied < 0) return -EFAULT;
    if (copied >= MAX_PATH_LEN - 1) return -ENAMETOOLONG;
    if (kpath[0] == '\0' && !(flags & AT_EMPTY_PATH)) return -ENOENT;
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = fdtable_get_current(dirfd);
        if (gfd < 0) return gfd;
        return vfs_fchown((int)gfd, uid, gid);
    }
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    return vfs_chownat(AT_FDCWD, full, uid, gid, flags);
}

int64_t sys_statx(int dirfd, const char *path, int flags, unsigned mask, void *buf) {
    if (!path || !buf) return -EFAULT;
    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_SYNC_TYPE))
        return -EINVAL;
    if (mask == 0) mask = STATX_BASIC_STATS;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r;

    /* LoongArch libc converts fstat(fd) → statx(fd,"",AT_EMPTY_PATH,…).
     * An empty path with AT_EMPTY_PATH means "stat the fd itself". */
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = fdtable_get_current(dirfd);
        if (gfd < 0) return gfd;
        r = vfs_fstat(gfd, &kst);
    } else {
        char full[MAX_PATH_LEN];
        int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
        if (pr < 0) return pr;
        r = vfs_fstatat(AT_FDCWD, full, &kst, flags);
        if (r == 0)
            kstat_apply_script_exec(full, &kst);
    }
    if (r < 0) return r;

    /* struct statx layout (256 bytes total):
     *   0:  stx_mask          u32
     *   4:  stx_blksize       u32
     *   8:  stx_attributes    u64
     *  16:  stx_nlink         u32
     *  20:  stx_uid           u32
     *  24:  stx_gid           u32
     *  28:  stx_mode          u16 + pad u16
     *  32:  stx_ino           u64
     *  40:  stx_size          u64
     *  48:  stx_blocks        u64
     *  56:  stx_attributes_mask u64
     *  64:  stx_atime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     *  80:  stx_btime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     *  96:  stx_ctime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     * 112:  stx_mtime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     * 128:  stx_rdev_major    u32
     * 132:  stx_rdev_minor    u32
     * 136:  stx_dev_major     u32
     * 140:  stx_dev_minor     u32
     * 144:  spare[14]         u64 × 14
     */
    uint64_t stx_buf64[256 / 8];
    memset(stx_buf64, 0, sizeof(stx_buf64));
    uint8_t *stx = (uint8_t *)stx_buf64;
    uint32_t *u32 = (uint32_t *)stx;
    uint64_t *u64 = (uint64_t *)stx;
    uint16_t *u16 = (uint16_t *)stx;

    uint32_t stx_mask = (mask & STATX_ALL) | STATX_BASIC_STATS;
    u32[0]  = stx_mask;
    u32[1]  = (uint32_t)kst.st_blksize;
    u32[4]  = kst.st_nlink;
    u32[5]  = kst.st_uid;
    u32[6]  = kst.st_gid;
    u16[14] = (uint16_t)user_visible_mode(kst.st_mode);
    u16[15] = 0;
    u64[4]  = kst.st_ino;
    u64[5]  = kst.st_size;
    u64[6]  = kst.st_blocks;

    *(int64_t *)(stx + 64)  = (int64_t)kst.st_atime;
    *(uint32_t *)(stx + 72) = (uint32_t)kst.st_atime_nsec;
    *(int64_t *)(stx + 80)  = (int64_t)kst.st_ctime;
    *(uint32_t *)(stx + 88) = (uint32_t)kst.st_ctime_nsec;
    *(int64_t *)(stx + 96)  = (int64_t)kst.st_ctime;
    *(uint32_t *)(stx + 104) = (uint32_t)kst.st_ctime_nsec;
    *(int64_t *)(stx + 112)  = (int64_t)kst.st_mtime;
    *(uint32_t *)(stx + 120) = (uint32_t)kst.st_mtime_nsec;

    u32[32] = (uint32_t)(kst.st_rdev >> 8);
    u32[33] = (uint32_t)(kst.st_rdev & 0xff) | (uint32_t)((kst.st_rdev >> 12) & 0xffffff00);
    u32[34] = (uint32_t)(kst.st_dev >> 8);
    u32[35] = (uint32_t)(kst.st_dev & 0xff) | (uint32_t)((kst.st_dev >> 12) & 0xffffff00);

    if (copy_to_user(buf, stx, 256) < 0) return -EFAULT;
    return 0;
}

int64_t sys_getdents64(int fd, void *dirp, size_t count) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    if (count > LINUX_IO_CHUNK_SIZE) count = LINUX_IO_CHUNK_SIZE;
    char *kbuf = proc_scratch_buffer(LINUX_IO_CHUNK_SIZE);
    if (!kbuf) return -ENOMEM;
    int64_t n = vfs_getdents64(gfd, kbuf, count);
    if (n > 0) {
        if (copy_to_user(dirp, kbuf, (size_t)n) < 0) return -EFAULT;
    }
    return n;
}

int64_t sys_linkat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath, int flags) {
    if (flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) return -EINVAL;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    char fold[MAX_PATH_LEN], fnew[MAX_PATH_LEN];
    int pr = syscall_path_at(olddirfd, kold, fold, sizeof(fold));
    if (pr < 0) return pr;
    pr = syscall_path_at(newdirfd, knew, fnew, sizeof(fnew));
    if (pr < 0) return pr;
    return vfs_link(fold, fnew);
}

int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    if (!target || !linkpath) return -EFAULT;
    char ktarget[MAX_PATH_LEN], klink[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(klink, linkpath, MAX_PATH_LEN) < 0) return -EFAULT;
    char flink[MAX_PATH_LEN];
    int pr = syscall_path_at(newdirfd, klink, flink, sizeof(flink));
    if (pr < 0) return pr;
    return vfs_symlink(ktarget, flink);
}

static void fill_statfs_buf(uint64_t sb[8], int fs_type)
{
    memset(sb, 0, sizeof(uint64_t) * 8);
    switch (fs_type) {
    case FS_TYPE_EXT4: sb[0] = EXT4_SUPER_MAGIC; break;
    case FS_TYPE_FAT32: sb[0] = 0x4d44; break;
    case FS_TYPE_PROCFS: sb[0] = 0x9fa0; break;
    case FS_TYPE_DEVFS: sb[0] = 0x01021994; break;
    case FS_TYPE_RAMFS:
    default: sb[0] = 0x858458f6; break;
    }
    sb[1] = PAGE_SIZE;
    sb[2] = 1024 * 1024;
    sb[3] = 512 * 1024;
    sb[4] = 512 * 1024;
    sb[5] = VFS_MAX_OPEN;
    sb[6] = VFS_MAX_OPEN / 2;
    sb[7] = MAX_NAME_LEN;
}

int64_t sys_statfs(const char *path, void *buf) {
    if (!buf) return -EFAULT;
    uint64_t sb[8];
    int fs_type = FS_TYPE_RAMFS;
    if (path) {
        char kpath[MAX_PATH_LEN];
        if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
        vnode_t *vn = vfs_resolve(kpath);
        if (!vn) return -ENOENT;
        if (vn->mnt) fs_type = vn->mnt->type;
        vnode_put(vn);
    }
    fill_statfs_buf(sb, fs_type);
    if (copy_to_user(buf, sb, 64) < 0) return -EFAULT;
    return 0;
}

int64_t sys_fstatfs(int fd, void *buf) {
    if (!buf) return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    int fs_type = FS_TYPE_RAMFS;
    if (vf->vnode && vf->vnode->mnt)
        fs_type = vf->vnode->mnt->type;
    vfs_put_file_ref((int)gfd, vf);
    uint64_t sb[8];
    fill_statfs_buf(sb, fs_type);
    return copy_to_user(buf, sb, 64) < 0 ? -EFAULT : 0;
}

int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags) {
    if (!target || !fstype) return -EFAULT;
    char ksrc[MAX_PATH_LEN], ktarget[MAX_PATH_LEN], kfstype[32];
    if (src) {
        if (user_strncpy(ksrc, src, MAX_PATH_LEN) < 0) return -EFAULT;
    } else {
        ksrc[0] = '\0';
    }
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(kfstype, fstype, 32) < 0) return -EFAULT;
    if (ktarget[0] != '/') {
        char abs[MAX_PATH_LEN];
        task_t *t = proc_current();
        const char *cwd = t ? t->fs.cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(abs, sizeof(abs), "/%s", ktarget);
        else
            snprintf(abs, sizeof(abs), "%s/%s", cwd, ktarget);
        strncpy(ktarget, abs, sizeof(ktarget) - 1);
        ktarget[sizeof(ktarget) - 1] = '\0';
    }
    return vfs_mount(ksrc, ktarget, kfstype, flags);
}

int64_t sys_umount2(const char *target, int flags) {
    (void)flags;
    if (!target) return -EFAULT;
    char ktarget[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (ktarget[0] != '/') {
        char abs[MAX_PATH_LEN];
        task_t *t = proc_current();
        const char *cwd = t ? t->fs.cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(abs, sizeof(abs), "/%s", ktarget);
        else
            snprintf(abs, sizeof(abs), "%s/%s", cwd, ktarget);
        strncpy(ktarget, abs, sizeof(ktarget) - 1);
        ktarget[sizeof(ktarget) - 1] = '\0';
    }
    return vfs_umount(ktarget);
}

int64_t sys_utimensat(int dirfd, const char *path, void *times, int flags) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    uint64_t ktimes[4];
    uint64_t *ptimes = NULL;
    if (times) {
        if (copy_from_user(ktimes, times, sizeof(ktimes)) < 0) return -EFAULT;
        ptimes = ktimes;
    }
    return vfs_utimensat(AT_FDCWD, full, ptimes, flags);
}
