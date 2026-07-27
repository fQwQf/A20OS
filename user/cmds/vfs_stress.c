#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_close_range
#define SYS_close_range 436
#endif

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

#ifndef SYS_mount
#define SYS_mount 40
#endif

#ifndef SYS_umount2
#define SYS_umount2 39
#endif

#ifndef SYS_symlinkat
#define SYS_symlinkat 36
#endif

#ifndef SYS_readlinkat
#define SYS_readlinkat 78
#endif

#ifndef SYS_openat
#define SYS_openat 56
#endif

#ifndef SYS_linkat
#define SYS_linkat 37
#endif

#ifndef SYS_fchmod
#define SYS_fchmod 91
#endif

#ifndef SYS_fchown
#define SYS_fchown 93
#endif

#ifndef SYS_statx
#define SYS_statx 291
#endif

#ifndef SYS_chroot
#define SYS_chroot 51
#endif

#ifndef SYS_faccessat
#define SYS_faccessat 48
#endif

static int fail(const char *what)
{
    printf("VFS_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int file_dup_close_range(void)
{
    const char *path = "/tmp/vfs_stress_io.txt";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("open-create");
    if (write(fd, "vfs", 3) != 3)
        return fail("write");
    if (lseek(fd, 0, SEEK_SET) < 0)
        return fail("lseek");
    char buf[8] = {0};
    if (read(fd, buf, 3) != 3 || strcmp(buf, "vfs") != 0)
        return fail("read-compare");

    int dupfd = dup3(fd, 32, 0);
    if (dupfd != 32)
        return fail("dup3");
    if (syscall(SYS_close_range, 32, 32, 0) < 0)
        return fail("close-range");
    errno = 0;
    if (write(dupfd, "x", 1) >= 0 || errno != EBADF)
        return fail("close-range-ebadf");
    if (close(fd) < 0)
        return fail("close");
    return 0;
}

static int rename_unlink_open(void)
{
    const char *old_path = "/tmp/vfs_stress_old.txt";
    const char *new_path = "/tmp/vfs_stress_new.txt";
    int fd = open(old_path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("rename-open-old");
    if (write(fd, "rename", 6) != 6)
        return fail("rename-write");
    close(fd);
    if (syscall(SYS_renameat2, AT_FDCWD, old_path, AT_FDCWD, new_path, 0) < 0)
        return fail("renameat2");
    if (open(old_path, O_RDONLY) >= 0)
        return fail("old-open-after-rename");
    fd = open(new_path, O_RDONLY);
    if (fd < 0)
        return fail("new-open-after-rename");
    char buf[8] = {0};
    if (read(fd, buf, 6) != 6 || strcmp(buf, "rename") != 0)
        return fail("rename-read");
    close(fd);
    if (unlink(new_path) < 0)
        return fail("unlink-new");
    return 0;
}

static int symlink_loop_boundary(void)
{
    unlink("/tmp/vfs_loop_a");
    unlink("/tmp/vfs_loop_b");
    if (syscall(SYS_symlinkat, "/tmp/vfs_loop_b", AT_FDCWD, "/tmp/vfs_loop_a") < 0)
        return fail("symlink-a");
    if (syscall(SYS_symlinkat, "/tmp/vfs_loop_a", AT_FDCWD, "/tmp/vfs_loop_b") < 0)
        return fail("symlink-b");
    char target[64] = {0};
    long n = syscall(SYS_readlinkat, AT_FDCWD, "/tmp/vfs_loop_a", target, sizeof(target) - 1);
    if (n <= 0)
        return fail("readlink-loop");
    target[n] = '\0';
    if (strcmp(target, "/tmp/vfs_loop_b") != 0)
        return fail("readlink-target");
    errno = 0;
    int fd = open("/tmp/vfs_loop_a", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return fail("open-symlink-loop");
    }
    if (errno != ELOOP && errno != ENOENT)
        return fail("symlink-loop-errno");
    unlink("/tmp/vfs_loop_a");
    unlink("/tmp/vfs_loop_b");
    return 0;
}

static int mount_umount_boundary(void)
{
    mkdir("/tmp/vfs_mount", 0755);
    errno = 0;
    long r = syscall(SYS_mount, "none", "/tmp/vfs_mount", "ramfs", 0, "");
    if (r == 0) {
        if (syscall(SYS_umount2, "/tmp/vfs_mount", 0) < 0)
            return fail("umount2");
        rmdir("/tmp/vfs_mount");
        return 0;
    }
    if (errno != ENOSYS && errno != EPERM && errno != EINVAL && errno != ENOENT)
        return fail("mount-boundary");
    rmdir("/tmp/vfs_mount");
    return 0;
}

/* concurrent_open_close: parent and child rapidly open/close the same file
 * to stress vnode reference counting and fdtable correctness. */
static int concurrent_open_close(void)
{
    const char *path = "/tmp/vfs_concurrent.txt";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("concurrent-open-create");
    if (write(fd, "concurrent", 10) != 10) {
        close(fd);
        return fail("concurrent-write");
    }
    close(fd);

    pid_t pid = fork();
    if (pid < 0)
        return fail("concurrent-fork");

    if (pid == 0) {
        for (int i = 0; i < 20; i++) {
            int cfd = open(path, O_RDONLY);
            if (cfd < 0)
                _exit(1);
            char buf[16] = {0};
            if (read(cfd, buf, 10) != 10 || strcmp(buf, "concurrent") != 0) {
                close(cfd);
                _exit(2);
            }
            close(cfd);
        }
        _exit(0);
    } else {
        for (int i = 0; i < 20; i++) {
            int pfd = open(path, O_RDONLY);
            if (pfd < 0)
                return fail("concurrent-parent-open");
            int dfd = dup(pfd);
            if (dfd < 0) {
                close(pfd);
                return fail("concurrent-parent-dup");
            }
            close(pfd);
            close(dfd);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("concurrent-wait");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("concurrent-child-fail");
    }
    unlink(path);
    return 0;
}

/* concurrent_rename_unlink: one process renames while another opens to
 * stress dcache invalidation and path resolution races. */
static int concurrent_rename_unlink(void)
{
    const char *path_a = "/tmp/vfs_conc_a.txt";
    const char *path_b = "/tmp/vfs_conc_b.txt";
    int fd = open(path_a, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("cren-open-a");
    if (write(fd, "race", 4) != 4) {
        close(fd);
        return fail("cren-write");
    }
    close(fd);

    pid_t pid = fork();
    if (pid < 0)
        return fail("cren-fork");

    if (pid == 0) {
        for (int i = 0; i < 10; i++) {
            if (syscall(SYS_renameat2, AT_FDCWD, path_a, AT_FDCWD, path_b, 0) < 0)
                _exit(1);
            if (syscall(SYS_renameat2, AT_FDCWD, path_b, AT_FDCWD, path_a, 0) < 0)
                _exit(2);
        }
        _exit(0);
    } else {
        for (int i = 0; i < 30; i++) {
            int pfd = open(path_a, O_RDONLY);
            if (pfd < 0)
                pfd = open(path_b, O_RDONLY);
            if (pfd >= 0)
                close(pfd);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("cren-wait");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("cren-child-fail");
    }
    unlink(path_a);
    unlink(path_b);
    return 0;
}

static int openat_relative(void)
{
    const char *dir = "/tmp/vfs_openat_dir";
    const char *name = "relative.txt";
    rmdir(dir);
    mkdir(dir, 0755);

    int dfd = syscall(SYS_openat, AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        rmdir(dir);
        return fail("openat-dir");
    }

    int fd = syscall(SYS_openat, dfd, name, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        close(dfd);
        rmdir(dir);
        return fail("openat-relative");
    }
    if (write(fd, "relative", 8) != 8) {
        close(fd);
        close(dfd);
        unlinkat(dfd, name, 0);
        rmdir(dir);
        return fail("openat-write");
    }
    close(fd);

    fd = syscall(SYS_openat, dfd, name, O_RDONLY);
    if (fd < 0) {
        close(dfd);
        unlinkat(dfd, name, 0);
        rmdir(dir);
        return fail("openat-reopen");
    }
    char buf[16] = {0};
    if (read(fd, buf, 8) != 8 || strcmp(buf, "relative") != 0) {
        close(fd);
        close(dfd);
        unlinkat(dfd, name, 0);
        rmdir(dir);
        return fail("openat-read");
    }
    close(fd);

    unlinkat(dfd, name, 0);
    close(dfd);
    rmdir(dir);
    return 0;
}

static int linkat_boundary(void)
{
    const char *path = "/tmp/vfs_link_src.txt";
    const char *linkname = "/tmp/vfs_link_dst.txt";
    unlink(path);
    unlink(linkname);

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("linkat-open");
    if (write(fd, "link", 4) != 4) {
        close(fd);
        return fail("linkat-write");
    }
    close(fd);

    if (syscall(SYS_linkat, AT_FDCWD, path, AT_FDCWD, linkname, 0) < 0)
        return fail("linkat");

    fd = open(linkname, O_RDONLY);
    if (fd < 0)
        return fail("linkat-open-dst");
    char buf[8] = {0};
    if (read(fd, buf, 4) != 4 || strcmp(buf, "link") != 0) {
        close(fd);
        return fail("linkat-read");
    }
    close(fd);

    unlink(path);
    unlink(linkname);
    return 0;
}

static int chmod_chown_boundary(void)
{
    const char *path = "/tmp/vfs_chmod.txt";
    unlink(path);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("chmod-open");

    if (syscall(SYS_fchmod, fd, 0600) < 0) {
        close(fd);
        return fail("fchmod");
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return fail("chmod-stat");
    }
    if ((st.st_mode & 0777) != 0600) {
        close(fd);
        return fail("chmod-mode");
    }

    errno = 0;
    if (syscall(SYS_fchown, fd, 0, 0) < 0) {
        if (errno != EPERM && errno != ENOSYS) {
            close(fd);
            return fail("fchown-errno");
        }
    }

    close(fd);
    unlink(path);
    return 0;
}

#ifndef STATX_TYPE
#define STATX_TYPE 0x00000001U
#define STATX_SIZE 0x00000002U
#define STATX_MODE 0x00000004U
#endif

struct statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t __spare1[14];
    uint64_t __spare2[12];
};

static int statx_boundary(void)
{
    const char *path = "/tmp/vfs_statx.txt";
    unlink(path);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("statx-open");
    if (write(fd, "statx", 5) != 5) {
        close(fd);
        return fail("statx-write");
    }
    close(fd);

    struct statx sx;
    memset(&sx, 0, sizeof(sx));
    long r = syscall(SYS_statx, AT_FDCWD, path, 0,
                     STATX_TYPE | STATX_SIZE | STATX_MODE, &sx);
    if (r < 0) {
        if (errno == ENOSYS) {
            unlink(path);
            return 0;
        }
        unlink(path);
        return fail("statx");
    }
    if (!(sx.stx_mask & STATX_SIZE) || sx.stx_size != 5) {
        unlink(path);
        return fail("statx-size");
    }
    if (!(sx.stx_mask & STATX_MODE) || (sx.stx_mode & 0777) != 0644) {
        unlink(path);
        return fail("statx-mode");
    }

    unlink(path);
    return 0;
}

static int chroot_boundary(void)
{
    const char *dir = "/tmp/vfs_chroot_dir";
    const char *name = "inside.txt";
    rmdir(dir);
    mkdir(dir, 0755);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        rmdir(dir);
        return fail("chroot-create");
    }
    if (write(fd, "inside", 6) != 6) {
        close(fd);
        unlink(path);
        rmdir(dir);
        return fail("chroot-write");
    }
    close(fd);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(path);
        rmdir(dir);
        return fail("chroot-fork");
    }
    if (pid == 0) {
        errno = 0;
        if (syscall(SYS_chroot, dir) < 0) {
            if (errno == EPERM || errno == ENOSYS)
                _exit(0);
            _exit(1);
        }
        fd = open("/inside.txt", O_RDONLY);
        if (fd < 0)
            _exit(2);
        char buf[8] = {0};
        if (read(fd, buf, 6) != 6 || strcmp(buf, "inside") != 0) {
            close(fd);
            _exit(3);
        }
        close(fd);

        int rootfd = syscall(SYS_openat, AT_FDCWD, "/", O_RDONLY | O_DIRECTORY);
        if (rootfd < 0)
            _exit(4);
        if (mkdirat(rootfd, "subdir", 0755) < 0)
            _exit(5);
        int subfd = syscall(SYS_openat, rootfd, "subdir",
                            O_RDONLY | O_DIRECTORY);
        if (subfd < 0)
            _exit(6);
        fd = syscall(SYS_openat, subfd, "before.txt",
                     O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fd < 0)
            _exit(7);
        if (write(fd, "dirfd", 5) != 5)
            _exit(8);
        close(fd);

        struct stat st;
        if (fstatat(subfd, "before.txt", &st, 0) < 0 || st.st_size != 5)
            _exit(9);
        if (syscall(SYS_renameat2, subfd, "before.txt",
                    subfd, "after.txt", 0) < 0)
            _exit(10);
        if (unlinkat(subfd, "after.txt", 0) < 0)
            _exit(11);
        close(subfd);
        if (unlinkat(rootfd, "subdir", AT_REMOVEDIR) < 0)
            _exit(12);
        close(rootfd);
        _exit(0);
    }

    int status;
    if (waitpid(pid, &status, 0) != pid) {
        unlink(path);
        rmdir(dir);
        return fail("chroot-wait");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("VFS_STRESS: chroot child status=%d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        unlink(path);
        rmdir(dir);
        return fail("chroot-child");
    }

    unlink(path);
    rmdir(dir);
    return 0;
}

static int symlink_relative_target(void)
{
    const char *dir = "/tmp/vfs_symlink_dir";
    const char *target = "target.txt";
    const char *linkname = "link.txt";
    rmdir(dir);
    mkdir(dir, 0755);

    int dfd = syscall(SYS_openat, AT_FDCWD, dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        rmdir(dir);
        return fail("symlink-dir");
    }

    int fd = syscall(SYS_openat, dfd, target, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        close(dfd);
        rmdir(dir);
        return fail("symlink-target-open");
    }
    if (write(fd, "target", 6) != 6) {
        close(fd);
        close(dfd);
        rmdir(dir);
        return fail("symlink-target-write");
    }
    close(fd);

    if (syscall(SYS_symlinkat, target, dfd, linkname) < 0) {
        close(dfd);
        rmdir(dir);
        return fail("symlinkat-relative");
    }

    char buf[64] = {0};
    long n = syscall(SYS_readlinkat, dfd, linkname, buf, sizeof(buf) - 1);
    if (n < 0 || (size_t)n != strlen(target) || strcmp(buf, target) != 0) {
        close(dfd);
        rmdir(dir);
        return fail("readlinkat-relative");
    }

    fd = syscall(SYS_openat, dfd, linkname, O_RDONLY);
    if (fd < 0) {
        close(dfd);
        rmdir(dir);
        return fail("symlink-open");
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 6) != 6 || strcmp(buf, "target") != 0) {
        close(fd);
        close(dfd);
        rmdir(dir);
        return fail("symlink-read-through");
    }
    close(fd);

    unlinkat(dfd, linkname, 0);
    unlinkat(dfd, target, 0);
    close(dfd);
    rmdir(dir);
    return 0;
}

int main(void)
{
    printf("VFS_STRESS: start\n");
    mkdir("/tmp", 0755);
    if (file_dup_close_range() != 0)
        return 1;
    if (rename_unlink_open() != 0)
        return 1;
    if (symlink_loop_boundary() != 0)
        return 1;
    if (mount_umount_boundary() != 0)
        return 1;
    if (concurrent_open_close() != 0)
        return 1;
    if (concurrent_rename_unlink() != 0)
        return 1;
    if (openat_relative() != 0)
        return 1;
    if (linkat_boundary() != 0)
        return 1;
    if (chmod_chown_boundary() != 0)
        return 1;
    if (statx_boundary() != 0)
        return 1;
    if (chroot_boundary() != 0)
        return 1;
    if (symlink_relative_target() != 0)
        return 1;
    printf("VFS_STRESS: PASS\n");
    return 0;
}
