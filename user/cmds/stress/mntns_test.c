/*
 * mntns_test — mount namespace smoke test.
 *
 * Coverage:
 *  - /proc/self/ns/mnt reads back in "mnt:[ino]" form
 *  - fork() shares the mount namespace (same ino)
 *  - unshare(CLONE_NEWPID) and other unimplemented ns types fail with EINVAL
 *  - unshare(CLONE_NEWNS) gives a private namespace (new ino)
 *  - mount() inside the namespace is invisible to a process in the parent
 *    namespace, and visible to a forked child (shared namespace)
 *  - setns() via /proc/<pid>/ns/mnt moves the caller between namespaces
 *  - setns() to a non-mnt namespace file is refused with EINVAL
 *
 * Prints "MNTNS_TEST: PASS" on success.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_unshare
#define SYS_unshare 97
#endif
#ifndef SYS_setns
#define SYS_setns 268
#endif

#define CLONE_NEWNS     0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS    0x04000000
#define CLONE_NEWIPC    0x08000000
#define CLONE_NEWUSER   0x10000000
#define CLONE_NEWPID    0x20000000
#define CLONE_NEWNET    0x40000000

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("MNTNS_TEST: FAIL: %s (line %d, errno=%d)\n", \
               msg, __LINE__, errno); \
        failures++; \
        goto out; \
    } \
} while (0)

static long xunshare(int flags) { return syscall(SYS_unshare, flags); }
static long xsetns(int fd, int nstype) { return syscall(SYS_setns, fd, nstype); }

/* Read an ns file and require the exact "<prefix>:[<number>]" shape. */
static int read_ns_file(const char *path, const char *prefix,
                        unsigned long long *ino_out)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char expect[16];
    snprintf(expect, sizeof(expect), "%s:[", prefix);
    if (strncmp(buf, expect, strlen(expect)) != 0)
        return -1;
    char *end = NULL;
    unsigned long long ino = strtoull(buf + strlen(expect), &end, 10);
    if (!end || *end != ']')
        return -1;
    if (ino_out)
        *ino_out = ino;
    return 0;
}

static int wait_exit0(pid_t pid)
{
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int main(void)
{
    unsigned long long init_ino = 0, child_ino = 0, new_ino = 0;
    int pipe_to_witness[2] = { -1, -1 };
    pid_t witness = -1;
    int fd_newns = -1, fd_initns = -1, fd_pidns = -1;

    /* The mount point lives on the shared rootfs so both namespaces have it. */
    if (mkdir("/tmp/ns_test", 0755) < 0 && errno != EEXIST) {
        printf("MNTNS_TEST: FAIL: mkdir /tmp/ns_test errno=%d\n", errno);
        return 1;
    }

    /* 1. /proc/self/ns/mnt format. */
    CHECK(read_ns_file("/proc/self/ns/mnt", "mnt", &init_ino) == 0,
          "read /proc/self/ns/mnt format");
    CHECK(init_ino != 0, "init namespace ino is nonzero");

    /* Other namespace types report singleton identifiers. */
    CHECK(read_ns_file("/proc/self/ns/pid", "pid", NULL) == 0,
          "read /proc/self/ns/pid format");
    CHECK(read_ns_file("/proc/self/ns/net", "net", NULL) == 0,
          "read /proc/self/ns/net format");

    /* 2. fork() shares the mount namespace. */
    {
        int pfd[2];
        CHECK(pipe(pfd) == 0, "pipe for fork-share check");
        pid_t c = fork();
        CHECK(c >= 0, "fork for share check");
        if (c == 0) {
            unsigned long long ino = 0;
            int ok = read_ns_file("/proc/self/ns/mnt", "mnt", &ino) == 0;
            (void)!write(pfd[1], &ino, sizeof(ino));
            _exit(ok ? 0 : 1);
        }
        close(pfd[1]);
        CHECK(read(pfd[0], &child_ino, sizeof(child_ino)) ==
              (ssize_t)sizeof(child_ino), "read child ino");
        close(pfd[0]);
        CHECK(wait_exit0(c) == 0, "child ns read");
        CHECK(child_ino == init_ino, "fork shares mount namespace");
    }

    /* 3. Honest errors for unimplemented namespace types. */
    errno = 0;
    CHECK(xunshare(CLONE_NEWPID) == -1 && errno == EINVAL,
          "unshare(CLONE_NEWPID) must fail with EINVAL");
    errno = 0;
    CHECK(xunshare(CLONE_NEWNET) == -1 && errno == EINVAL,
          "unshare(CLONE_NEWNET) must fail with EINVAL");
    errno = 0;
    CHECK(xunshare(CLONE_NEWUSER) == -1 && errno == EINVAL,
          "unshare(CLONE_NEWUSER) must fail with EINVAL");

    /* 4. Witness child stays in the initial namespace; it checks (on
     *    request) whether the namespace-private mount is visible there. */
    CHECK(pipe(pipe_to_witness) == 0, "pipe for witness");
    witness = fork();
    CHECK(witness >= 0, "fork witness");
    if (witness == 0) {
        char token;
        if (read(pipe_to_witness[0], &token, 1) != 1)
            _exit(2);
        /* In the initial namespace /tmp/ns_test is the empty rootfs dir. */
        _exit(access("/tmp/ns_test/from_child", F_OK) == 0 ? 1 : 0);
    }
    close(pipe_to_witness[0]);
    pipe_to_witness[0] = -1;

    /* 5. unshare(CLONE_NEWNS) creates a private namespace. */
    CHECK(xunshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS)");
    CHECK(read_ns_file("/proc/self/ns/mnt", "mnt", &new_ino) == 0,
          "read new namespace ino");
    CHECK(new_ino != init_ino, "unshare creates a distinct namespace");

    /* 6. Mount ramfs inside the private namespace only. */
    CHECK(mount("none", "/tmp/ns_test", "ramfs", 0, NULL) == 0,
          "mount ramfs in child namespace");
    {
        int fd = open("/tmp/ns_test/from_child", O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0, "create file on namespaced mount");
        close(fd);
    }

    /* 7. A forked child shares the new namespace and sees the mount. */
    {
        pid_t c = fork();
        CHECK(c >= 0, "fork after unshare");
        if (c == 0) {
            unsigned long long ino = 0;
            if (read_ns_file("/proc/self/ns/mnt", "mnt", &ino) < 0)
                _exit(2);
            if (ino != new_ino)
                _exit(3);
            _exit(access("/tmp/ns_test/from_child", F_OK) == 0 ? 0 : 4);
        }
        CHECK(wait_exit0(c) == 0, "forked child sees namespaced mount");
    }

    /* 8. The witness (initial namespace) must not see the mount. */
    CHECK(write(pipe_to_witness[1], "x", 1) == 1, "signal witness");
    close(pipe_to_witness[1]);
    pipe_to_witness[1] = -1;
    CHECK(wait_exit0(witness) == 0,
          "namespaced mount invisible in parent namespace");
    witness = -1;

    /* 9. setns() between namespaces through /proc/<pid>/ns/mnt fds. */
    {
        char procpath[64];
        snprintf(procpath, sizeof(procpath), "/proc/%d/ns/mnt", (int)getppid());
        fd_newns = open("/proc/self/ns/mnt", O_RDONLY);
        CHECK(fd_newns >= 0, "open own ns fd");
        fd_initns = open(procpath, O_RDONLY);
        CHECK(fd_initns >= 0, "open parent ns fd");

        CHECK(xsetns(fd_initns, CLONE_NEWNS) == 0, "setns to parent namespace");
        errno = 0;
        CHECK(access("/tmp/ns_test/from_child", F_OK) == -1 && errno == ENOENT,
              "after setns the namespaced mount is gone");
        {
            unsigned long long back_ino = 0;
            CHECK(read_ns_file("/proc/self/ns/mnt", "mnt", &back_ino) == 0 &&
                  back_ino == init_ino, "setns restored the initial namespace");
        }

        CHECK(xsetns(fd_newns, CLONE_NEWNS) == 0, "setns back to own namespace");
        CHECK(access("/tmp/ns_test/from_child", F_OK) == 0,
              "after setns back the mount is visible again");

        /* nstype mismatch must be refused. */
        errno = 0;
        CHECK(xsetns(fd_initns, CLONE_NEWPID) == -1 && errno == EINVAL,
              "setns with mismatched nstype fails with EINVAL");
    }

    /* 10. setns() to non-mnt namespace files is honestly refused. */
    fd_pidns = open("/proc/self/ns/pid", O_RDONLY);
    CHECK(fd_pidns >= 0, "open pid ns fd");
    errno = 0;
    CHECK(xsetns(fd_pidns, 0) == -1 && errno == EINVAL,
          "setns to pid namespace fails with EINVAL");
    errno = 0;
    CHECK(xsetns(fd_pidns, CLONE_NEWPID) == -1 && errno == EINVAL,
          "setns to pid namespace with matching type still fails");

    /* 11. clone(CLONE_NEWNS) creates a namespace for the child. */
    {
        pid_t c = (pid_t)syscall(SYS_clone, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0);
        CHECK(c >= 0, "clone(CLONE_NEWNS)");
        if (c == 0) {
            unsigned long long ino = 0;
            int ok = read_ns_file("/proc/self/ns/mnt", "mnt", &ino) == 0 &&
                     ino != new_ino;
            _exit(ok ? 0 : 1);
        }
        CHECK(wait_exit0(c) == 0, "clone(CLONE_NEWNS) child has new namespace");
    }

    /* 12. umount of the namespace-private mount only affects this ns. */
    CHECK(umount2("/tmp/ns_test", 0) == 0, "umount namespaced ramfs");
    errno = 0;
    CHECK(access("/tmp/ns_test/from_child", F_OK) == -1 && errno == ENOENT,
          "umount detached the namespaced mount");

out:
    if (witness > 0) {
        if (pipe_to_witness[1] >= 0) {
            (void)!write(pipe_to_witness[1], "x", 1);
            close(pipe_to_witness[1]);
        }
        int st;
        (void)waitpid(witness, &st, 0);
    }
    if (fd_newns >= 0) close(fd_newns);
    if (fd_initns >= 0) close(fd_initns);
    if (fd_pidns >= 0) close(fd_pidns);
    if (failures)
        return 1;
    printf("MNTNS_TEST: PASS\n");
    return 0;
}
