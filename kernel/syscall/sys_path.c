#include "syscall_internal.h"

int64_t sys_mkdirat(int dirfd, const char *path, int mode) {
    (void)dirfd;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_mkdir(kpath, mode);
}

int64_t sys_unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    if (flags & AT_REMOVEDIR) return vfs_rmdir(kpath);
    return vfs_unlink(kpath);
}

int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags) {
    (void)olddir; (void)newdir; (void)flags;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_rename(kold, knew);
}

int64_t sys_chdir(const char *path) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_chdir(kpath);
}

int64_t sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -EFAULT;
    size_t len = strlen(t->cwd) + 1;
    if (size < len) return -ERANGE;
    if (copy_to_user(buf, t->cwd, len) < 0) return -EFAULT;
    return (int64_t)len;
}

static void copy_kstat_to_user(void *st, const kstat_t *kst) {
    uint64_t buf64[128 / 8];
    memset(buf64, 0, sizeof(buf64));
    uint8_t *buf = (uint8_t *)buf64;
    uint64_t *u64 = (uint64_t *)buf;
    uint32_t *u32 = (uint32_t *)buf;
    u64[0]  = kst->st_dev;
    u64[1]  = kst->st_ino;
    u32[4]  = kst->st_mode;
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
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    kstat_t kst;
    int r = vfs_fstat(gfd, &kst);
    if (r < 0) return r;
    if (!st) return -EFAULT;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags) {
    (void)dirfd; (void)flags;
    if (!path || !st) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r = vfs_fstatat(dirfd, kpath, &kst, flags);
    if (r < 0) return r;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    if (sz > 4096) sz = 4096;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t r = vfs_readlinkat(dirfd, kpath, kbuf, sz);
    if (r > 0) {
        if (copy_to_user(buf, kbuf, (size_t)r) < 0) { kfree(kbuf); return -EFAULT; }
    }
    kfree(kbuf);
    return r;
}

int64_t sys_faccessat(int dirfd, const char *path, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_faccessat(dirfd, kpath, mode);
}

int64_t sys_statx(int dirfd, const char *path, int flags, unsigned mask, void *buf) {
    if (!path || !buf) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r;

    /* LoongArch libc converts fstat(fd) → statx(fd,"",AT_EMPTY_PATH,…).
     * An empty path with AT_EMPTY_PATH means "stat the fd itself". */
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = syscall_get_global_fd(dirfd);
        if (gfd < 0) return gfd;
        r = vfs_fstat(gfd, &kst);
    } else {
        r = vfs_fstatat(dirfd, kpath, &kst, flags);
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

    u32[0]  = (uint32_t)(mask | STATX_BASIC_STATS);
    u32[1]  = (uint32_t)kst.st_blksize;
    u32[4]  = kst.st_nlink;
    u32[5]  = kst.st_uid;
    u32[6]  = kst.st_gid;
    u16[14] = (uint16_t)kst.st_mode;
    u16[15] = 0;
    u64[4]  = kst.st_ino;
    u64[5]  = kst.st_size;
    u64[6]  = kst.st_blocks;

    *(int64_t *)(stx + 64)  = (int64_t)kst.st_atime;
    *(uint32_t *)(stx + 72) = (uint32_t)kst.st_atime_nsec;
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
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    if (count > 4096) count = 4096;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t n = vfs_getdents64(gfd, kbuf, count);
    if (n > 0) {
        if (copy_to_user(dirp, kbuf, (size_t)n) < 0) { kfree(kbuf); return -EFAULT; }
    }
    kfree(kbuf);
    return n;
}

int64_t sys_linkat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath, int flags) {
    (void)olddirfd; (void)newdirfd; (void)flags;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_link(kold, knew);
}

int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    (void)newdirfd;
    if (!target || !linkpath) return -EFAULT;
    char ktarget[MAX_PATH_LEN], klink[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(klink, linkpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_symlink(ktarget, klink);
}

int64_t sys_statfs(const char *path, void *buf) {
    (void)path;
    if (!buf) return -EFAULT;
    uint64_t sb[8];
    memset(sb, 0, sizeof(sb));
    sb[0] = 0x4006;
    sb[1] = 4096;
    sb[2] = 1024*1024;
    sb[3] = 512*1024;
    sb[4] = 512*1024;
    if (copy_to_user(buf, sb, 64) < 0) return -EFAULT;
    return 0;
}

int64_t sys_fstatfs(int fd, void *buf) {
    (void)fd;
    return sys_statfs(NULL, buf);
}

int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags) {
    if (!src || !target || !fstype) return -EFAULT;
    char ksrc[MAX_PATH_LEN], ktarget[MAX_PATH_LEN], kfstype[32];
    if (user_strncpy(ksrc, src, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(kfstype, fstype, 32) < 0) return -EFAULT;
    return vfs_mount(ksrc, ktarget, kfstype, flags);
}

int64_t sys_umount2(const char *target, int flags) {
    (void)flags;
    if (!target) return -EFAULT;
    char ktarget[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_umount(ktarget);
}

int64_t sys_utimensat(int dirfd, const char *path, void *times, int flags) {
    (void)dirfd; (void)path; (void)times; (void)flags;
    return 0;
}

