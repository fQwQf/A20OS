#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct open_how_test { unsigned long long flags, mode, resolve; };
#define CHECK(x) do { if (!(x)) { printf("SYMLINK_MOUNT: FAIL line=%d errno=%d %s\n", __LINE__, errno, #x); return 1; } } while (0)

static int contents(int fd) {
    if (fd < 0) return 0;
    char buf[8] = {0};
    int ok = read(fd, buf, sizeof(buf)) == 6 && !memcmp(buf, "marker", 6);
    close(fd);
    return ok;
}

int main(void) {
    char base[128], path[256], target[256];
    snprintf(base, sizeof(base), "/tmp/symlink-mount-%d", getpid());
    CHECK(mkdir(base, 0755) == 0);
    CHECK(mount("none", base, "tmpfs", 0, NULL) == 0);
    int dir = open(base, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    CHECK(mkdirat(dir, "inside", 0755) == 0);
    int fd = openat(dir, "inside/marker", O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0 && write(fd, "marker", 6) == 6);
    close(fd);
    snprintf(target, sizeof(target), "%s/inside", base);
    CHECK(symlinkat(target, dir, "absolute") == 0);
    CHECK(symlinkat("inside", dir, "relative") == 0);
    CHECK(symlinkat("/proc/uptime", dir, "cross") == 0);
    CHECK(symlinkat("/inside", dir, "jailed") == 0);
    snprintf(path, sizeof(path), "%s/absolute/marker", base);
    CHECK(contents(open(path, O_RDONLY)));
    CHECK(contents(openat(dir, "relative/marker", O_RDONLY)));
    struct open_how_test how = {0};
    CHECK(contents(syscall(SYS_openat2, dir, "absolute/marker", &how, sizeof(how))));
    CHECK(contents(syscall(SYS_openat2, dir, "relative/marker", &how, sizeof(how))));
    fd = openat(dir, "cross", O_RDONLY);
    CHECK(fd >= 0);
    char byte;
    CHECK(read(fd, &byte, 1) == 1 && byte >= '0' && byte <= '9');
    close(fd);
    how.resolve = 1; /* RESOLVE_NO_XDEV */
    CHECK(syscall(SYS_openat2, dir, "cross", &how, sizeof(how)) < 0 && errno == EXDEV);
    how.resolve = 8; /* RESOLVE_BENEATH forbids absolute symlinks. */
    CHECK(syscall(SYS_openat2, dir, "absolute/marker", &how, sizeof(how)) < 0 && errno == EXDEV);
    how.resolve = 16; /* RESOLVE_IN_ROOT anchors absolute links at dir. */
    CHECK(contents(syscall(SYS_openat2, dir, "jailed/marker", &how, sizeof(how))));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (chroot(base) || chdir("/") || !contents(open("/jailed/marker", O_RDONLY)))
            _exit(1);
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    unlinkat(dir, "absolute", 0); unlinkat(dir, "relative", 0);
    unlinkat(dir, "cross", 0); unlinkat(dir, "jailed", 0);
    unlinkat(dir, "inside/marker", 0); unlinkat(dir, "inside", AT_REMOVEDIR);
    close(dir);
    CHECK(umount(base) == 0 && rmdir(base) == 0);
    puts("SYMLINK_MOUNT: PASS");
    return 0;
}
