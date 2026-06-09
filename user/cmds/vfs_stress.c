#include <errno.h>
#include <fcntl.h>
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
    printf("VFS_STRESS: PASS\n");
    return 0;
}
