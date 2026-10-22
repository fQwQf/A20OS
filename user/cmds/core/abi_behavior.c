/*
 * Normalized ABI behaviour fixture (Linux personality).
 *
 * Prints stable `ABI_BEHAVIOR: key=value` lines for one operation set so the
 * same fixture can run under ABI=linux and ABI=both kernels (ABI-01), and so
 * its output can be diffed against the Native-personality twin
 * (user/tests/test_native_abi_behavior.c, ABI-03).  The expected values live
 * in the smoke case, not in either implementation: a divergence on one side
 * fails the case instead of silently redefining the answer.
 *
 * Operations, in this order: pipe byte stream, fd table, anonymous mmap plus
 * mprotect, socketpair round trip, level-triggered readiness, invalid
 * arguments.  Nondeterministic values (descriptor numbers, addresses, pids)
 * never appear in the output.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int emit(const char *line)
{
    if (printf("ABI_BEHAVIOR: %s\n", line) < 0)
        return 1;
    return fflush(stdout) == 0 ? 0 : 1;
}

int main(void)
{
    const char *path = "/tmp/abi-behavior.txt";
    /* Identity tag for the differential extractor; the normalized
     * `ABI_BEHAVIOR:` lines below are identical on both personalities. */
    if (emit("run=linux"))
        return 1;

    /* pipe: two writes joined into one byte stream, drained in two reads. */
    int p[2];
    if (pipe(p) != 0)
        return 2;
    if (write(p[1], "hello ", 6) != 6 || write(p[1], "world", 5) != 5)
        return 3;
    char buf[16] = {0};
    if (read(p[0], buf, 6) != 6 || read(p[0], buf + 6, 10) != 5 ||
        memcmp(buf, "hello world", 11) != 0)
        return 4;
    close(p[0]);
    close(p[1]);
    if (emit("pipe partial=6 rest=5 joined=hello world"))
        return 5;

    /* fd table: create, write, seek, read back, dup, close-then-use.  A dup
     * shares the open file description, so it shares the offset too. */
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0)
        return 6;
    if (write(fd, "abcdef", 6) != 6 || lseek(fd, 0, SEEK_SET) != 0)
        return 7;
    char fb[16] = {0};
    if (read(fd, fb, 6) != 6 || memcmp(fb, "abcdef", 6) != 0)
        return 8;
    int dupfd = dup(fd);
    if (dupfd < 0)
        return 9;
    if (lseek(dupfd, 2, SEEK_SET) != 2)
        return 10;
    char part[8] = {0};
    if (read(fd, part, 3) != 3 || memcmp(part, "cde", 3) != 0)
        return 11;
    close(dupfd);
    if (read(fd, part, 1) != 1)
        return 12;
    int closed = fd;
    close(fd);
    errno = 0;
    if (!(read(closed, part, 1) == -1 && errno == EBADF))
        return 13;
    if (emit("fd_table write=6 seek=0 read=6 dup_shared_offset=ok "
             "close_then_read=bad_handle"))
        return 14;

    /* mmap: anonymous RW page, write and read it, then protect read-only. */
    unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return 15;
    memset(page, 0x5a, 4096);
    if (page[0] != 0x5a || page[4095] != 0x5a)
        return 16;
    if (mprotect(page, 4096, PROT_READ) != 0 || page[7] != 0x5a)
        return 17;
    munmap(page, 4096);
    if (emit("mmap write_read=ok protect_readonly=ok"))
        return 18;

    /* socketpair: one message each way. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 19;
    if (write(sv[0], "ping", 4) != 4)
        return 20;
    char s[8] = {0};
    if (read(sv[1], s, sizeof(s)) != 4 || memcmp(s, "ping", 4) != 0)
        return 21;
    close(sv[0]);
    close(sv[1]);
    if (emit("socketpair send=4 recv=4 data=ping"))
        return 22;

    /* Level-triggered readiness: still readable after a partial read. */
    int q[2];
    if (pipe(q) != 0)
        return 23;
    if (write(q[1], "level", 5) != 5)
        return 24;
    struct pollfd pfd = {.fd = q[0], .events = POLLIN};
    char lb[8] = {0};
    if (poll(&pfd, 1, 1000) != 1 || read(q[0], lb, 2) != 2)
        return 25;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) != 1 || read(q[0], lb, sizeof(lb)) != 3)
        return 26;
    pfd.revents = 0;
    if (poll(&pfd, 1, 20) != 0)
        return 27;
    close(q[0]);
    close(q[1]);
    if (emit("readiness level=ok drained=ok"))
        return 28;

    /* Invalid-argument class. */
    errno = 0;
    if (!(fcntl(-1, F_GETFD) == -1 && errno == EBADF))
        return 29;
    errno = 0;
    if (!(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
              MAP_FAILED &&
          errno == EINVAL))
        return 30;
    unlink(path);
    if (emit("errors bad_descriptor=bad_handle zero_length=invalid"))
        return 31;

    if (emit("PASS"))
        return 32;
    return 0;
}
