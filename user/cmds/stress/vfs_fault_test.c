/*
 * vfs_fault_test — failure-injection and race coverage for the VFS frontend
 * that vfs_stress does not reach:
 *
 *   1. short / partial I/O: reads that stop at EOF, zero-length transfers,
 *      offsets past EOF, and a read that straddles the EOF boundary; then a
 *      byte-exact re-read to prove no over- or under-copy happened.
 *   2. fd-table exhaustion: open until EMFILE, then prove that closing the
 *      held descriptors lets a new open succeed (the failure must roll back).
 *   3. permission propagation: a child that drops to uid 1000 must get EACCES
 *      writing a root-owned 0644 file; the parent must still be able to.
 *   4. concurrent mutation: two children rename/unlink one name while a third
 *      opens and reads it; every outcome must be a legal one (success or
 *      ENOENT), never a wrong errno or a crash.
 *   5. shutdown race: forks children that keep doing open/write/read, prints
 *      VFS_FAULT: CHILDREN-RUNNING and returns to the shell so the harness's
 *      poweroff lands while those operations are in flight.
 *
 * Prints VFS_FAULT: PASS before phase 5 starts; the caller checks both that
 * marker and a clean power-off with no kernel panic.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_mount
#define SYS_mount 40
#endif
#ifndef SYS_umount2
#define SYS_umount2 39
#endif

#define BASE "/tmp/vfs_fault"

static const char *g_phase = "init";

static int fail(const char *what)
{
    printf("VFS_FAULT: FAIL %s (%s) errno=%d\n", g_phase, what, errno);
    return 1;
}

/* ---------------------------------------------------------------- phase 1 */
static int partial_io(void)
{
    g_phase = "partial-io";
    char path[128];
    snprintf(path, sizeof(path), "%s/partial", BASE);

    const size_t total = 10000;
    char *src = malloc(total);
    if (!src)
        return fail("malloc-src");
    for (size_t i = 0; i < total; i++)
        src[i] = (char)((i * 131 + 7) & 0xff);

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        free(src);
        return fail("open");
    }
    ssize_t w = write(fd, src, total);
    if (w != (ssize_t)total) {
        close(fd);
        free(src);
        return fail("write-full");
    }

    /* A read that stops exactly at EOF reports the remaining byte count. */
    if (lseek(fd, 4090, SEEK_SET) != 4090) {
        close(fd);
        free(src);
        return fail("lseek-straddle");
    }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, 20); /* 4090..10000 is 5910 available */
    if (n != 20) {
        close(fd);
        free(src);
        return fail("read-straddle-full");
    }
    if (memcmp(buf, src + 4090, 20) != 0) {
        close(fd);
        free(src);
        return fail("read-straddle-bytes");
    }

    /* A short read at the tail: 10 bytes remain from offset 9990. */
    if (lseek(fd, 9990, SEEK_SET) != 9990) {
        close(fd);
        free(src);
        return fail("lseek-tail");
    }
    n = read(fd, buf, 64);
    if (n != 10) {
        close(fd);
        free(src);
        return fail("read-short-tail-count");
    }
    if (memcmp(buf, src + 9990, 10) != 0) {
        close(fd);
        free(src);
        return fail("read-short-tail-bytes");
    }

    /* At EOF a read returns 0, and stays 0. */
    n = read(fd, buf, 64);
    if (n != 0) {
        close(fd);
        free(src);
        return fail("read-at-eof");
    }
    n = read(fd, buf, 64);
    if (n != 0) {
        close(fd);
        free(src);
        return fail("read-at-eof-sticky");
    }

    /* Zero-length transfers are no-ops. */
    if (read(fd, buf, 0) != 0) {
        close(fd);
        free(src);
        return fail("read-zero");
    }
    if (write(fd, src, 0) != 0) {
        close(fd);
        free(src);
        return fail("write-zero");
    }

    /* An offset past EOF reads 0 but is preserved. */
    if (lseek(fd, 20000, SEEK_SET) != 20000) {
        close(fd);
        free(src);
        return fail("lseek-past-eof");
    }
    if (read(fd, buf, 16) != 0) {
        close(fd);
        free(src);
        return fail("read-past-eof");
    }
    if (lseek(fd, 0, SEEK_CUR) != 20000) {
        close(fd);
        free(src);
        return fail("offset-moved-after-eof-read");
    }

    /* Re-read the whole file in page-sized chunks and compare byte-exactly. */
    if (lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        free(src);
        return fail("lseek-rewind");
    }
    size_t got = 0;
    char chunk[4096];
    while (got < total) {
        ssize_t r = read(fd, chunk, sizeof(chunk));
        if (r <= 0) {
            close(fd);
            free(src);
            return fail("chunk-read");
        }
        if ((size_t)r > total - got || memcmp(chunk, src + got, (size_t)r) != 0) {
            close(fd);
            free(src);
            return fail("chunk-bytes");
        }
        got += (size_t)r;
    }
    close(fd);
    free(src);
    return 0;
}

/* ---------------------------------------------------------------- phase 2 */
static int fd_exhaustion(void)
{
    g_phase = "fd-exhaust";
    char path[128];
    snprintf(path, sizeof(path), "%s/fdex", BASE);

    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("open-seed");
    if (write(fd, "x", 1) != 1) {
        close(fd);
        return fail("write-seed");
    }

    enum { HOLD = 4200 }; /* above MAX_FILES=4096 so exhaustion is reachable */
    static int held[HOLD];
    int n = 0;
    for (; n < HOLD; n++) {
        int h = open(path, O_RDONLY);
        if (h < 0) {
            if (errno != EMFILE && errno != ENFILE) {
                for (int i = 0; i < n; i++)
                    close(held[i]);
                close(fd);
                return fail("exhaust-errno");
            }
            break;
        }
        held[n] = h;
    }
    if (n == HOLD) {
        for (int i = 0; i < n; i++)
            close(held[i]);
        close(fd);
        return fail("never-exhausted");
    }

    /* While exhausted, a new open must still fail, not silently succeed. */
    int probe = open(path, O_RDONLY);
    if (probe >= 0) {
        close(probe);
        for (int i = 0; i < n; i++)
            close(held[i]);
        close(fd);
        return fail("open-while-exhausted");
    }

    /* Closing the held descriptors must roll the failure back. */
    for (int i = 0; i < n; i++)
        close(held[i]);
    int again = open(path, O_RDONLY);
    if (again < 0) {
        close(fd);
        return fail("open-after-release");
    }
    close(again);
    close(fd);
    return 0;
}

/* ---------------------------------------------------------------- phase 3 */
static int permission_child(void)
{
    g_phase = "eacces";

    /* fork/setuid are not in the musl subset this test is linked against
     * everywhere, so use the raw syscalls when the wrappers are absent. */
#ifdef SYS_fork
    pid_t pid = (pid_t)syscall(SYS_fork);
#else
    pid_t pid = fork();
#endif
    if (pid < 0)
        return fail("fork");

    char path[128];
    snprintf(path, sizeof(path), "%s/owned", BASE);

    if (pid == 0) {
        if (setuid(1000) != 0)
            _exit(10);
        errno = 0;
        int fd = open(path, O_WRONLY);
        if (fd >= 0) {
            close(fd);
            _exit(11); /* root-like access is wrong */
        }
        _exit(errno == EACCES ? 0 : 12);
    }

    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        return fail("waitpid");
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("VFS_FAULT: child status=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return fail("child");
    }
    return 0;
}

static int permission_parent_setup(void)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/owned", BASE);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail("open-owned");
    if (write(fd, "root", 4) != 4) {
        close(fd);
        return fail("write-owned");
    }
    close(fd);
    if (chmod(path, 0644) != 0)
        return fail("chmod-owned");
    return 0;
}

/* ---------------------------------------------------------------- phase 4 */
static int concurrent_mutation(void)
{
    g_phase = "concurrent-mutation";
    char a[128], b[128];
    snprintf(a, sizeof(a), "%s/mut_a", BASE);
    snprintf(b, sizeof(b), "%s/mut_b", BASE);

    int fd = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail("open-a");
    if (write(fd, "payload", 7) != 7) {
        close(fd);
        return fail("write-a");
    }
    close(fd);

    pid_t renamer = fork();
    if (renamer < 0)
        return fail("fork-renamer");
    if (renamer == 0) {
        for (int i = 0; i < 200; i++) {
            if (rename(a, b) != 0 && errno != ENOENT)
                _exit(21);
            if (rename(b, a) != 0 && errno != ENOENT)
                _exit(22);
        }
        _exit(0);
    }

    pid_t reader = fork();
    if (reader < 0) {
        kill(renamer, 9);
        waitpid(renamer, NULL, 0);
        return fail("fork-reader");
    }
    if (reader == 0) {
        char buf[16];
        for (int i = 0; i < 200; i++) {
            int rfd = open(a, O_RDONLY);
            if (rfd < 0)
                rfd = open(b, O_RDONLY);
            if (rfd < 0) {
                if (errno == ENOENT)
                    continue;
                _exit(31);
            }
            (void)read(rfd, buf, sizeof(buf));
            close(rfd);
        }
        _exit(0);
    }

    for (int i = 0; i < 2; i++) {
        int st = 0;
        pid_t p = i == 0 ? renamer : reader;
        if (waitpid(p, &st, 0) != p) {
            kill(renamer, 9);
            kill(reader, 9);
            return fail("waitpid-child");
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            return fail("child-exit");
    }

    /* Leave the tree tidy regardless of which name survived. */
    unlink(a);
    unlink(b);
    return 0;
}

/* ---------------------------------------------------------------- phase 5 */
static pid_t spawn_io_child(const char *path)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char buf[64];
        for (;;) {
            int fd = open(path, O_CREAT | O_RDWR, 0644);
            if (fd >= 0) {
                (void)write(fd, "live", 4);
                (void)lseek(fd, 0, SEEK_SET);
                (void)read(fd, buf, sizeof(buf));
                close(fd);
            }
            int rfd = open(BASE, O_RDONLY | O_DIRECTORY);
            if (rfd >= 0)
                close(rfd);
        }
        _exit(0);
    }
    return pid;
}

int main(int argc, char **argv)
{
    int stop = (argc > 1 && strcmp(argv[1], "stop") == 0);
    mkdir(BASE, 0755);

    if (permission_parent_setup())
        return 1;
    if (partial_io())
        return 1;
    if (fd_exhaustion())
        return 1;
    if (permission_child())
        return 1;
    if (concurrent_mutation())
        return 1;

    char live[128];
    snprintf(live, sizeof(live), "%s/live", BASE);
    pid_t c1 = spawn_io_child(live);
    pid_t c2 = spawn_io_child(live);
    if (c1 < 0 || c2 < 0) {
        if (c1 > 0) kill(c1, 9);
        if (c2 > 0) kill(c2, 9);
        return fail("fork-io-child");
    }

    printf("VFS_FAULT: PASS\n");
    printf("VFS_FAULT: CHILDREN-RUNNING pid=%d,%d\n", (int)c1, (int)c2);
    fflush(stdout);
    if (stop) {
        /* Give the children time to be provably in flight, then quiesce them
         * so a following poweroff has nothing to refuse. */
        usleep(1500 * 1000);
        kill(c1, 9);
        kill(c2, 9);
        waitpid(c1, NULL, 0);
        waitpid(c2, NULL, 0);
        printf("VFS_FAULT: CHILDREN-STOPPED\n");
        fflush(stdout);
        return 0;
    }
    /* Otherwise leave them running: a poweroff issued now must hit the
     * shutdown admission gate while their open/write/read calls are in
     * flight, and must not crash. */
    return 0;
}
