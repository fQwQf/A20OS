/* Force a real Native UFSD death after a durable open-unlink, then remount. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *what)
{
    dprintf(2, "UFS_ORPHAN_RECOVERY: FAIL %s errno=%d\n", what, errno);
    _exit(1);
}
#define CHECK(c, w) do { if (!(c)) fail(w); } while (0)
static void deadline(int signal_number)
{
    (void)signal_number;
    static const char text[] = "UFS_ORPHAN_RECOVERY: FAIL operation deadline\n";
    (void)write(2, text, sizeof(text) - 1);
    _exit(1);
}
static pid_t start_service(char **args)
{
    pid_t pid = fork();
    CHECK(pid >= 0, "fork service");
    if (!pid) {
        execl(args[1], args[1], args[2], args[3], "ext4", (char *)0);
        _exit(90);
    }
    for (int i = 0; i < 500; ++i) {
        struct statfs fs;
        if (statfs(args[2], &fs) == 0 && (unsigned long)fs.f_type == 0x55584653UL)
            return pid;
        usleep(10000);
    }
    fail("Native service mount");
    return -1;
}
static void verify_volume(const char *root)
{
    const char *token = getenv("FS_ORPHAN_EXPECT_VOLUME_TOKEN");
    CHECK(token && strlen(token) == 64, "independent volume token required");
    char path[544], data[128];
    CHECK(snprintf(path, sizeof(path), "%s/a20-orphan-gate-volume", root) < (int)sizeof(path), "volume path");
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open volume token");
    ssize_t length = read(fd, data, sizeof(data));
    CHECK(length == 64 && !memcmp(data, token, 64), "intended ext4 volume");
    CHECK(close(fd) == 0, "close volume token");
}
int main(int argc, char **argv)
{
    CHECK(argc == 4, "usage: ufs_orphan_recovery_test <service> <mount> <block-index>");
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, deadline);
    alarm(40);
    pid_t service = start_service(argv);
    verify_volume(argv[2]);
    char path[544], alias[544], sentinel[544];
    CHECK(snprintf(path, sizeof(path), "%s/death-orphan", argv[2]) < (int)sizeof(path), "orphan path");
    CHECK(snprintf(alias, sizeof(alias), "%s/death-alias", argv[2]) < (int)sizeof(alias), "alias path");
    CHECK(snprintf(sentinel, sizeof(sentinel), "%s/death-survivor", argv[2]) < (int)sizeof(sentinel), "survivor path");
    static const char payload[] = "survives-service-death\n";
    int keep = open(sentinel, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(keep >= 0, "create linked survivor");
    CHECK(write(keep, payload, sizeof(payload)) == sizeof(payload) && fsync(keep) == 0 && close(keep) == 0, "persist linked survivor");
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0, "create orphan");
    char data[16384];memset(data, 0x5a, sizeof(data));
    CHECK(write(fd, data, sizeof(data)) == sizeof(data) && fsync(fd) == 0, "persist original data");
    CHECK(link(path, alias) == 0, "hard link before death");
    CHECK(unlink(path) == 0 && unlink(alias) == 0, "unlink both names while open");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_nlink == 0, "zero-link orphan before death");
    unsigned long long orphan = (unsigned long long)st.st_ino;
    CHECK(fsync(fd) == 0, "persist orphan chain before kill");
    CHECK(kill(service, SIGKILL) == 0, "kill actual Native service");
    int status;
    CHECK(waitpid(service, &status, 0) == service && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "observe actual service death");
    errno = 0;
    CHECK(fstat(fd, &st) == -1 && errno == EIO, "dead service metadata fails EIO");
    errno = 0;
    CHECK(close(fd) == -1 && errno == EIO, "dead service close reports EIO");
    errno = 0;
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF, "failed close consumes descriptor");
    CHECK(umount2(argv[2], 0) == 0, "detach dead service mount");
    service = start_service(argv);
    verify_volume(argv[2]);
    CHECK(stat(path, &st) == -1 && errno == ENOENT && stat(alias, &st) == -1 && errno == ENOENT, "removed names stay absent after recovery");
    keep = open(sentinel, O_RDONLY);
    CHECK(keep >= 0, "open survivor after recovery");
    CHECK(read(keep, data, sizeof(data)) == sizeof(payload) && !memcmp(data, payload, sizeof(payload)) && close(keep) == 0, "linked survivor unchanged");
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0 && write(fd, payload, sizeof(payload)) == sizeof(payload) && fsync(fd) == 0 && close(fd) == 0, "fresh service accepts durable writes");
    CHECK(unlink(path) == 0 && unlink(sentinel) == 0, "remove recovery fixtures");
    CHECK(umount2(argv[2], 0) == 0, "clean unmount recovered service");
    CHECK(waitpid(service, &status, 0) == service && WIFEXITED(status) && WEXITSTATUS(status) == 0, "recovered service exits cleanly");
    alarm(0);
    printf("UFS_ORPHAN_RECOVERY: PASS old_ino=%llu death=SIGKILL stale=EIO consumed=EBADF\n", orphan);
    return 0;
}
