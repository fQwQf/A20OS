#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

#ifndef SYS_statx
#define SYS_statx 291
#endif

#ifndef RESOLVE_NO_XDEV
#define RESOLVE_NO_XDEV        0x01
#define RESOLVE_NO_MAGICLINKS  0x02
#define RESOLVE_NO_SYMLINKS    0x04
#define RESOLVE_BENEATH        0x08
#define RESOLVE_IN_ROOT        0x10
#define RESOLVE_NO_TRAILING_SYMLINKS 0x20
#define RESOLVE_CACHED         0x40
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#define RENAME_EXCHANGE  2
#endif

#ifndef STATX_TYPE
#define STATX_TYPE 0x00000001U
#define STATX_SIZE 0x00000002U
#define STATX_MODE 0x00000004U
#endif

#ifndef AT_STATX_SYNC_AS_STAT
#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC   0x2000
#define AT_STATX_DONT_SYNC    0x4000
#endif

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

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

static int fail(const char *what)
{
    printf("VFS_EDGE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

#ifndef SYS_ioctl
#define SYS_ioctl 29
#endif

#define EDGE_KDSETMODE    0x4B3A
#define EDGE_KDGETMODE    0x4B3B
#define EDGE_KDGKBMODE    0x4B44
#define EDGE_KDSKBMODE    0x4B45
#define EDGE_KDGKBTYPE    0x4B33
#define EDGE_VT_OPENQRY   0x5600
#define EDGE_VT_GETMODE   0x5601
#define EDGE_VT_SETMODE   0x5602
#define EDGE_VT_GETSTATE  0x5603
#define EDGE_VT_ACTIVATE  0x5606
#define EDGE_VT_WAITACTIVE 0x5607
#define EDGE_KD_TEXT      0x00
#define EDGE_KD_GRAPHICS  0x01
#define EDGE_K_XLATE      0x01
#define EDGE_K_RAW        0x00

struct edge_vt_mode {
    char mode;
    char waitv;
    short relsig;
    short acqsig;
    short frsig;
};

struct edge_vt_stat {
    unsigned short v_active;
    unsigned short v_signal;
    unsigned short v_state;
};

/* /dev/tty0 exposes single-VT console ioctls so display servers (weston)
 * can switch the console to graphics/raw mode without patching userspace. */
static int tty0_console_ioctls(void)
{
    int fd = open("/dev/tty0", O_RDWR | O_NONBLOCK);
    if (fd < 0)
        return fail("tty0-open");

    int mode = -1;
    if (syscall(SYS_ioctl, fd, EDGE_KDGKBMODE, &mode) < 0 || mode != EDGE_K_XLATE) {
        close(fd);
        return fail("tty0-kdgkbmode");
    }
    if (syscall(SYS_ioctl, fd, EDGE_KDSKBMODE, EDGE_K_RAW) < 0) {
        close(fd);
        return fail("tty0-kdskbmode-raw");
    }
    mode = -1;
    if (syscall(SYS_ioctl, fd, EDGE_KDGKBMODE, &mode) < 0 || mode != EDGE_K_RAW) {
        close(fd);
        return fail("tty0-kdgkbmode-raw");
    }

    if (syscall(SYS_ioctl, fd, EDGE_KDSETMODE, EDGE_KD_GRAPHICS) < 0) {
        close(fd);
        return fail("tty0-kdsetmode-gfx");
    }
    mode = -1;
    if (syscall(SYS_ioctl, fd, EDGE_KDGETMODE, &mode) < 0 || mode != EDGE_KD_GRAPHICS) {
        close(fd);
        return fail("tty0-kdgetmode-gfx");
    }
    if (syscall(SYS_ioctl, fd, EDGE_KDSETMODE, EDGE_KD_TEXT) < 0) {
        close(fd);
        return fail("tty0-kdsetmode-text");
    }

    char kbtype = 0;
    if (syscall(SYS_ioctl, fd, EDGE_KDGKBTYPE, &kbtype) < 0 || kbtype != 0x02) {
        close(fd);
        return fail("tty0-kdgkbtype");
    }

    int free_vt = 0;
    if (syscall(SYS_ioctl, fd, EDGE_VT_OPENQRY, &free_vt) < 0 || free_vt != 1) {
        close(fd);
        return fail("tty0-vt-openqry");
    }
    struct edge_vt_mode vm;
    memset(&vm, 0, sizeof(vm));
    if (syscall(SYS_ioctl, fd, EDGE_VT_GETMODE, &vm) < 0) {
        close(fd);
        return fail("tty0-vt-getmode");
    }
    vm.mode = 1; /* VT_PROCESS */
    if (syscall(SYS_ioctl, fd, EDGE_VT_SETMODE, &vm) < 0) {
        close(fd);
        return fail("tty0-vt-setmode");
    }
    struct edge_vt_stat vs;
    memset(&vs, 0, sizeof(vs));
    if (syscall(SYS_ioctl, fd, EDGE_VT_GETSTATE, &vs) < 0 || vs.v_active != 1) {
        close(fd);
        return fail("tty0-vt-getstate");
    }
    if (syscall(SYS_ioctl, fd, EDGE_VT_ACTIVATE, 1) < 0) {
        close(fd);
        return fail("tty0-vt-activate");
    }
    if (syscall(SYS_ioctl, fd, EDGE_VT_WAITACTIVE, 1) < 0) {
        close(fd);
        return fail("tty0-vt-waitactive");
    }
    close(fd);
    return 0;
}

static int openat2_beneath(void)
{
    const char *dir = "/tmp/vfs_edge_beneath";
    const char *name = "file.txt";
    rmdir(dir);
    mkdir(dir, 0755);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        rmdir(dir);
        return fail("beneath-create");
    }
    close(fd);

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        unlink(path);
        rmdir(dir);
        return fail("beneath-opendir");
    }

    struct open_how how = { .flags = O_RDONLY, .mode = 0, .resolve = RESOLVE_BENEATH };
    errno = 0;
    long r = syscall(SYS_openat2, dfd, name, &how, sizeof(how));
    if (r < 0) {
        close(dfd);
        unlink(path);
        rmdir(dir);
        return fail("openat2-beneath-relative");
    }
    close((int)r);

    errno = 0;
    how.flags = O_RDONLY;
    how.resolve = RESOLVE_BENEATH;
    r = syscall(SYS_openat2, dfd, "../vfs_edge_beneath", &how, sizeof(how));
    if (r >= 0) {
        close((int)r);
        close(dfd);
        unlink(path);
        rmdir(dir);
        return fail("openat2-beneath-escape-should-fail");
    }
    if (errno != EXDEV && errno != EACCES && errno != EPERM && errno != ENOENT)
        return fail("openat2-beneath-escape-errno");

    close(dfd);
    unlink(path);
    rmdir(dir);
    return 0;
}

static int openat2_beneath_symlink_escape(void)
{
    const char *base = "/tmp/vfs_edge_beneath2";
    const char *dir = "/tmp/vfs_edge_beneath2/dir";
    const char *outside = "/tmp/vfs_edge_beneath2/outside";
    const char *file = "/tmp/vfs_edge_beneath2/outside/file.txt";

    unlink(file);
    rmdir(outside);
    rmdir(dir);
    rmdir(base);

    if (mkdir(base, 0755) < 0 && errno != EEXIST) return fail("beneath-sym-base");
    if (mkdir(dir, 0755) < 0) return fail("beneath-sym-dir");
    if (mkdir(outside, 0755) < 0) return fail("beneath-sym-outside");

    int fd = open(file, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        rmdir(outside);
        rmdir(dir);
        rmdir(base);
        return fail("beneath-sym-create");
    }
    close(fd);

    char lpath[128];
    snprintf(lpath, sizeof(lpath), "%s/escape", dir);
    if (symlink("/tmp/vfs_edge_beneath2/outside", lpath) < 0) {
        unlink(file);
        rmdir(outside);
        rmdir(dir);
        rmdir(base);
        return fail("beneath-sym-link");
    }

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        unlink(lpath);
        unlink(file);
        rmdir(outside);
        rmdir(dir);
        rmdir(base);
        return fail("beneath-sym-opendir");
    }

    struct open_how how = { .flags = O_RDONLY, .mode = 0, .resolve = RESOLVE_BENEATH };
    errno = 0;
    long r = syscall(SYS_openat2, dfd, "escape/file.txt", &how, sizeof(how));
    if (r >= 0) {
        close((int)r);
        close(dfd);
        unlink(lpath);
        unlink(file);
        rmdir(outside);
        rmdir(dir);
        rmdir(base);
        return fail("openat2-beneath-symlink-escape-should-fail");
    }
    if (errno != EXDEV && errno != EACCES && errno != EPERM && errno != ENOENT) {
        close(dfd);
        unlink(lpath);
        unlink(file);
        rmdir(outside);
        rmdir(dir);
        rmdir(base);
        return fail("openat2-beneath-symlink-escape-errno");
    }

    close(dfd);
    unlink(lpath);
    unlink(file);
    rmdir(outside);
    rmdir(dir);
    rmdir(base);
    return 0;
}

static int openat2_no_symlinks(void)
{
    const char *dir = "/tmp/vfs_edge_nosym";
    const char *target = "target.txt";
    const char *linkname = "link.txt";
    rmdir(dir);
    mkdir(dir, 0755);

    char tpath[128], lpath[128];
    snprintf(tpath, sizeof(tpath), "%s/%s", dir, target);
    snprintf(lpath, sizeof(lpath), "%s/%s", dir, linkname);

    int fd = open(tpath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        rmdir(dir);
        return fail("nosym-create");
    }
    close(fd);
    if (symlink(target, lpath) < 0) {
        unlink(tpath);
        rmdir(dir);
        return fail("nosym-symlink");
    }

    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        unlink(lpath);
        unlink(tpath);
        rmdir(dir);
        return fail("nosym-opendir");
    }

    struct open_how how = { .flags = O_RDONLY, .mode = 0, .resolve = RESOLVE_NO_SYMLINKS };
    errno = 0;
    long r = syscall(SYS_openat2, dfd, linkname, &how, sizeof(how));
    if (r >= 0) {
        close((int)r);
        close(dfd);
        unlink(lpath);
        unlink(tpath);
        rmdir(dir);
        return fail("nosym-should-fail");
    }
    if (errno != ELOOP && errno != EMLINK)
        return fail("nosym-errno");

    close(dfd);
    unlink(lpath);
    unlink(tpath);
    rmdir(dir);
    return 0;
}

static int renameat2_flags(void)
{
    const char *a = "/tmp/vfs_edge_a.txt";
    const char *b = "/tmp/vfs_edge_b.txt";
    const char *c = "/tmp/vfs_edge_c.txt";
    unlink(a);
    unlink(b);
    unlink(c);

    int fd = open(a, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return fail("rename-create-a");
    if (write(fd, "a", 1) != 1) {
        close(fd);
        return fail("rename-write-a");
    }
    close(fd);

    fd = open(c, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return fail("rename-create-c");
    close(fd);

    errno = 0;
    if (syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, c, RENAME_NOREPLACE) >= 0) {
        unlink(a);
        unlink(c);
        return fail("rename-noreplace-over-existing-should-fail");
    }
    if (errno != EEXIST) {
        unlink(a);
        unlink(c);
        return fail("rename-noreplace-over-existing-errno");
    }

    errno = 0;
    if (syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE) < 0) {
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-noreplace-new");
    }

    /* b now contains 'a'; recreate a with distinct 'A' content for exchange test. */
    fd = open(a, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-recreate-a2");
    }
    if (write(fd, "A", 1) != 1) {
        close(fd);
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-write-a2");
    }
    close(fd);

    errno = 0;
    if (syscall(SYS_renameat2, AT_FDCWD, a, AT_FDCWD, b, RENAME_EXCHANGE) < 0) {
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-exchange");
    }

    fd = open(a, O_RDONLY);
    if (fd < 0) {
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-exchange-open-a");
    }
    char buf[8] = {0};
    if (read(fd, buf, 1) != 1 || buf[0] != 'a') {
        close(fd);
        return fail("rename-exchange-content-a");
    }
    close(fd);

    fd = open(b, O_RDONLY);
    if (fd < 0) {
        unlink(a);
        unlink(b);
        unlink(c);
        return fail("rename-exchange-open-b");
    }
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 1) != 1 || buf[0] != 'A') {
        close(fd);
        return fail("rename-exchange-content-b");
    }
    close(fd);

    unlink(a);
    unlink(b);
    unlink(c);
    return 0;
}

static int statx_sync_mask(void)
{
    const char *path = "/tmp/vfs_edge_statx.txt";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return fail("statx-create");
    if (write(fd, "statx", 5) != 5) {
        close(fd);
        return fail("statx-write");
    }
    close(fd);

    struct statx sx;
    memset(&sx, 0, sizeof(sx));
    long r = syscall(SYS_statx, AT_FDCWD, path, AT_STATX_FORCE_SYNC,
                     STATX_TYPE | STATX_SIZE | STATX_MODE, &sx);
    if (r < 0) {
        if (errno == ENOSYS) {
            unlink(path);
            return 0;
        }
        unlink(path);
        return fail("statx-force-sync");
    }
    if (!(sx.stx_mask & STATX_SIZE) || sx.stx_size != 5) {
        unlink(path);
        return fail("statx-size");
    }

    memset(&sx, 0, sizeof(sx));
    r = syscall(SYS_statx, AT_FDCWD, path, AT_STATX_DONT_SYNC,
                STATX_TYPE | STATX_SIZE | STATX_MODE, &sx);
    if (r < 0) {
        unlink(path);
        return fail("statx-dont-sync");
    }
    if (!(sx.stx_mask & STATX_SIZE) || sx.stx_size != 5) {
        unlink(path);
        return fail("statx-dont-sync-size");
    }

    unlink(path);
    return 0;
}

static int xattr_namespace(void)
{
    const char *path = "/tmp/vfs_edge_xattr.txt";
    unlink(path);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return fail("xattr-create");
    close(fd);

    const char *value = "value";
    size_t vlen = strlen(value);
    errno = 0;
    long r = syscall(SYS_setxattr, path, "user.vfs_edge", value, vlen, 0);
    if (r < 0) {
        if (errno != ENOSYS && errno != EOPNOTSUPP) {
            unlink(path);
            return fail("xattr-user-set");
        }
    }

    if (r >= 0) {
        char buf[32];
        r = syscall(SYS_getxattr, path, "user.vfs_edge", buf, sizeof(buf));
        if (r < 0) {
            unlink(path);
            return fail("xattr-user-get");
        }
        if ((size_t)r != vlen || memcmp(buf, value, vlen) != 0) {
            unlink(path);
            return fail("xattr-user-value");
        }
    }

    errno = 0;
    r = syscall(SYS_setxattr, path, "invalid.vfs_edge", value, vlen, 0);
    if (r >= 0) {
        unlink(path);
        return fail("xattr-invalid-should-fail");
    }
    if (errno != EOPNOTSUPP && errno != ENOSYS && errno != EPERM && errno != EINVAL)
        return fail("xattr-invalid-errno");

    errno = 0;
    r = syscall(SYS_setxattr, path, "trusted.vfs_edge", value, vlen, 0);
    if (r >= 0) {
        /* Unprivileged tasks may succeed if capability granted; just remove. */
        syscall(SYS_removexattr, path, "trusted.vfs_edge");
    } else if (errno != EPERM && errno != ENOSYS && errno != EOPNOTSUPP) {
        unlink(path);
        return fail("xattr-trusted-errno");
    }

    unlink(path);
    return 0;
}

static int symlink_loop_deep(void)
{
    const char *base = "/tmp/vfs_edge_loop";
    rmdir(base);
    mkdir(base, 0755);

    char prev[128], cur[128];
    snprintf(prev, sizeof(prev), "%s/link0", base);
    for (int i = 1; i <= 42; i++) {
        snprintf(cur, sizeof(cur), "%s/link%d", base, i);
        if (symlink(cur, prev) < 0) {
            for (int j = 0; j < i; j++) {
                snprintf(cur, sizeof(cur), "%s/link%d", base, j);
                unlink(cur);
            }
            rmdir(base);
            return fail("deep-loop-symlink");
        }
        strcpy(prev, cur);
    }

    errno = 0;
    int fd = open("/tmp/vfs_edge_loop/link0", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        for (int i = 0; i <= 42; i++) {
            snprintf(cur, sizeof(cur), "%s/link%d", base, i);
            unlink(cur);
        }
        rmdir(base);
        return fail("deep-loop-should-fail");
    }
    if (errno != ELOOP && errno != EMLINK) {
        for (int i = 0; i <= 42; i++) {
            snprintf(cur, sizeof(cur), "%s/link%d", base, i);
            unlink(cur);
        }
        rmdir(base);
        return fail("deep-loop-errno");
    }

    for (int i = 0; i <= 42; i++) {
        snprintf(cur, sizeof(cur), "%s/link%d", base, i);
        unlink(cur);
    }
    rmdir(base);
    return 0;
}

#ifndef SYS_mount
#define SYS_mount 40
#endif
#ifndef SYS_umount2
#define SYS_umount2 39
#endif

static int mount_dotdot_crossing(void)
{
    const char *mp = "/tmp/vfs_edge_mount";
    const char *marker = "/tmp/vfs_edge_mount/.mounted";
    rmdir(mp);
    mkdir(mp, 0755);

    errno = 0;
    long r = syscall(SYS_mount, "none", mp, "ramfs", 0, "");
    if (r < 0) {
        if (errno != ENOSYS && errno != EPERM && errno != EINVAL && errno != ENOENT)
            return fail("mount-cross-mount");
        rmdir(mp);
        return 0;
    }

    int fd = open(marker, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        syscall(SYS_umount2, mp, 0);
        rmdir(mp);
        return fail("mount-cross-marker");
    }
    close(fd);

    if (chdir(mp) < 0) {
        unlink(marker);
        syscall(SYS_umount2, mp, 0);
        rmdir(mp);
        return fail("mount-cross-chdir");
    }

    if (chdir("..") < 0) {
        unlink(marker);
        syscall(SYS_umount2, mp, 0);
        rmdir(mp);
        return fail("mount-cross-dotdot");
    }

    /* After cd .. from mount root we should be able to touch a sibling in /tmp. */
    fd = open("vfs_edge_mount_sibling", O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        unlink(marker);
        syscall(SYS_umount2, mp, 0);
        rmdir(mp);
        return fail("mount-cross-sibling-create");
    }
    close(fd);
    unlink("/tmp/vfs_edge_mount_sibling");

    chdir("/");
    unlink(marker);
    if (syscall(SYS_umount2, mp, 0) < 0) {
        rmdir(mp);
        return fail("mount-cross-umount");
    }
    rmdir(mp);
    return 0;
}

static int chroot_escape(void)
{
    const char *dir = "/tmp/vfs_edge_chroot";
    const char *marker = "/tmp/vfs_edge_chroot_escape_marker";
    unlink(marker);
    rmdir(dir);
    mkdir(dir, 0755);

    int mfd = open(marker, O_CREAT | O_RDWR, 0644);
    if (mfd < 0) {
        rmdir(dir);
        return fail("chroot-marker-create");
    }
    close(mfd);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(marker);
        rmdir(dir);
        return fail("chroot-fork");
    }

    if (pid == 0) {
        if (syscall(SYS_chroot, dir) < 0) {
            if (errno == EPERM || errno == ENOSYS)
                _exit(0);
            _exit(1);
        }
        if (chdir("/") < 0)
            _exit(2);
        /* Try to reach a sibling of the chroot dir via .. -- must fail. */
        errno = 0;
        int fd = open("../vfs_edge_chroot_escape_marker", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            _exit(3);
        }
        _exit(0);
    }

    int status;
    if (waitpid(pid, &status, 0) != pid) {
        unlink(marker);
        rmdir(dir);
        return fail("chroot-wait");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(marker);
        rmdir(dir);
        return fail("chroot-escape-detected");
    }

    unlink(marker);
    rmdir(dir);
    return 0;
}

int main(void)
{
    printf("VFS_EDGE: start\n");
    mkdir("/tmp", 0755);
    if (tty0_console_ioctls() != 0)
        return 1;
    if (openat2_beneath() != 0)
        return 1;
    if (openat2_beneath_symlink_escape() != 0)
        return 1;
    if (openat2_no_symlinks() != 0)
        return 1;
    if (renameat2_flags() != 0)
        return 1;
    if (statx_sync_mask() != 0)
        return 1;
    if (xattr_namespace() != 0)
        return 1;
    if (symlink_loop_deep() != 0)
        return 1;
    if (mount_dotdot_crossing() != 0)
        return 1;
    if (chroot_escape() != 0)
        return 1;
    printf("VFS_EDGE: PASS\n");
    return 0;
}
