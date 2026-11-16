#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *what) {
    dprintf(2, "FS_ORPHAN: FAIL %s errno=%d\n", what, errno);
    _exit(1);
}
#define CHECK(c, w) do { if (!(c)) fail(w); } while (0)
static void transfer(int fd, void *buffer, size_t length, off_t offset, int writing) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = writing ? pwrite(fd, (char *)buffer + done, length - done, offset + done)
                            : pread(fd, (char *)buffer + done, length - done, offset + done);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0, "complete file transfer");
        done += n;
    }
}
static struct stat metadata(int fd, nlink_t links) {
    struct stat st;
    CHECK(fstat(fd, &st) == 0, "fstat open inode");
    if (st.st_nlink != links) {
        dprintf(2, "FS_ORPHAN: nlink actual=%llu expected=%llu ino=%llu\n",
                (unsigned long long)st.st_nlink, (unsigned long long)links,
                (unsigned long long)st.st_ino);
        fail("disk link count through open fd");
    }
    return st;
}
static void message(int fd, char expected) {
    char byte;
    ssize_t n;
    do { n = read(fd, &byte, 1); } while (n < 0 && errno == EINTR);
    CHECK(n == 1 && byte == expected, "child handshake");
}
static void await_service(const char *root) {
    if (!getenv("FS_ORPHAN_EXPECT_UXFS")) return;
    struct timespec begin, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0, "service readiness clock");
    for (;;) {
        struct statfs fs;
        if (statfs(root, &fs) == 0 && (unsigned long)fs.f_type == 0x55584653UL) {
            const char *token = getenv("FS_ORPHAN_EXPECT_VOLUME_TOKEN");
            if (token) {
                char path[544], bytes[128];
                CHECK(strlen(token) < sizeof(bytes), "volume token size");
                CHECK(snprintf(path, sizeof(path), "%s/a20-orphan-gate-volume", root) < (int)sizeof(path), "volume path length");
                int fd = open(path, O_RDONLY);
                CHECK(fd >= 0, "open independent volume marker");
                ssize_t n = read(fd, bytes, sizeof(bytes));
                CHECK(n == (ssize_t)strlen(token) && !memcmp(bytes, token, (size_t)n), "Native mount owns intended ext4 volume");
                close(fd);
            }
            puts("FS_ORPHAN: mounted=UXFS");
            return;
        }
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "service readiness clock");
        CHECK(now.tv_sec - begin.tv_sec < 5, "Native filesystem ready within deadline");
        usleep(10000);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2, "usage: fs_orphan_lifecycle <private-test-root>");
    setvbuf(stdout, NULL, _IONBF, 0);
    await_service(argv[1]);
    char directory[512], first[544], alias[544];
    CHECK(snprintf(directory, sizeof(directory), "%s/a20-orphan-XXXXXX", argv[1]) < (int)sizeof(directory), "root path length");
    CHECK(mkdtemp(directory) != NULL, "private test directory");
    snprintf(first, sizeof(first), "%s/first", directory);
    snprintf(alias, sizeof(alias), "%s/alias", directory);
    int fd = open(first, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0, "create original");
    enum { BYTES = 6 * 4096 };
    unsigned char original[BYTES], copy[BYTES];
    for (unsigned i = 0; i < BYTES; i++) original[i] = (unsigned char)((i * 37 + 19) % 251);
    transfer(fd, original, BYTES, 0, 1);
    CHECK(fsync(fd) == 0, "sync original");
    struct stat initial = metadata(fd, 1);
    CHECK(link(first, alias) == 0, "create hard link");
    CHECK(metadata(fd, 2).st_ino == initial.st_ino, "hard link retains inode");
    int second = open(alias, O_RDWR), duplicate = dup(fd);
    CHECK(second >= 0 && duplicate >= 0, "independent open and dup");
    CHECK(metadata(second, 2).st_ino == initial.st_ino, "alias inode identity");
    CHECK(unlink(first) == 0, "unlink nonfinal name");
    CHECK(metadata(fd, 1).st_ino == initial.st_ino, "nonfinal unlink preserves inode");
    transfer(second, copy, BYTES, 0, 0);
    CHECK(memcmp(copy, original, BYTES) == 0, "hard link data survives nonfinal unlink");
    CHECK(unlink(alias) == 0, "unlink final name while open");
    CHECK(metadata(second, 0).st_ino == initial.st_ino, "open orphan inode identity");
    struct stat absent;
    errno = 0;
    CHECK(stat(alias, &absent) == -1 && errno == ENOENT, "final name absent");
    transfer(duplicate, copy, BYTES, 0, 0);
    CHECK(memcmp(copy, original, BYTES) == 0, "open orphan data survives unlink");
    int fresh = open(alias, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fresh >= 0 && metadata(fresh, 1).st_ino != initial.st_ino, "recreated name gets a distinct inode");
    CHECK(write(fresh, "new", 3) == 3 && fsync(fresh) == 0, "new name data");
    unsigned char changed = 0xa7;
    transfer(second, &changed, 1, 4097, 1);
    transfer(fd, copy, BYTES, 0, 0);
    original[4097] = changed;
    CHECK(memcmp(copy, original, BYTES) == 0, "old opens share orphan writes");
    CHECK(ftruncate(second, 8193) == 0 && metadata(fd, 0).st_size == 8193, "open orphan truncate");
    CHECK(fsync(second) == 0, "sync truncated orphan");
    int ready[2], go[2];
    CHECK(pipe(ready) == 0 && pipe(go) == 0, "fork pipes");
    pid_t child = fork();
    CHECK(child >= 0, "fork fd survivor");
    if (!child) {
        close(ready[0]); close(go[1]); close(fd); close(duplicate); close(fresh);
        CHECK(write(ready[1], "R", 1) == 1, "child ready");
        message(go[0], 'G');
        CHECK(metadata(second, 0).st_size == 8193, "child is last open owner");
        transfer(second, copy, 8193, 0, 0);
        CHECK(memcmp(copy, original, 8193) == 0, "child reads orphan after parent closes");
        CHECK(fsync(second) == 0 && close(second) == 0, "last orphan close");
        _exit(0);
    }
    close(ready[1]); close(go[0]);
    message(ready[0], 'R');
    CHECK(close(fd) == 0 && close(duplicate) == 0 && close(second) == 0, "parent drops all old references");
    CHECK(write(go[1], "G", 1) == 1, "release last owner");
    int status; pid_t got;
    do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
    CHECK(got == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "last owner exited");
    char new_data[3];
    transfer(fresh, new_data, sizeof(new_data), 0, 0);
    CHECK(memcmp(new_data, "new", 3) == 0 && metadata(fresh, 1).st_size == 3, "new inode survives orphan reap");
    CHECK(close(fresh) == 0 && unlink(alias) == 0, "remove recreated file");
    close(ready[0]); close(go[1]);
    CHECK(rmdir(directory) == 0, "remove private directory");
    sync();
    printf("FS_ORPHAN: PASS old_ino=%llu bytes=%u\n", (unsigned long long)initial.st_ino, BYTES);
    return 0;
}
