/*
 * unix_ch_test — Linux ABI over the internal IPC channel bridge.
 *
 * AF_UNIX socketpair(SOCK_STREAM/SOCK_SEQPACKET) data plane is backed by
 * the internal channel (kernel/ipc).  Verifies: message roundtrips,
 * stream chunking beyond the 64 KiB channel message size, partial reads,
 * nonblocking EAGAIN, poll POLLIN, and EOF on peer close — all through
 * plain POSIX syscalls, no A20-specific API.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define fail(code, msg) do { \
    printf("UNIX_CH: FAIL %s (errno=%d)\n", msg, errno); \
    return code; \
} while (0)

static int roundtrip(int type)
{
    int sv[2];
    if (socketpair(AF_UNIX, type, 0, sv) < 0)
        fail(1, "socketpair");

    char msg[] = "hello channel";
    if (send(sv[0], msg, sizeof(msg), 0) != (ssize_t)sizeof(msg))
        fail(2, "send");
    char buf[64] = {0};
    ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
    if (n != (ssize_t)sizeof(msg) || memcmp(buf, msg, sizeof(msg)) != 0)
        fail(3, "roundtrip mismatch");
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int stream_large_and_partial(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        fail(4, "socketpair");

    /* 200 KiB write: exceeds the 64 KiB channel message size, must be
     * chunked internally and reassembled by the stream reader. */
    static char big[200 * 1024];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char)(i & 0xff);
    if (write(sv[0], big, sizeof(big)) != (ssize_t)sizeof(big))
        fail(5, "large write");

    /* Partial read: 10 bytes first, then the rest. */
    char got[sizeof(big)];
    ssize_t n = read(sv[1], got, 10);
    if (n != 10)
        fail(6, "partial read");
    size_t total = 10;
    while (total < sizeof(big)) {
        n = read(sv[1], got + total, sizeof(big) - total);
        if (n <= 0)
            fail(7, "large read");
        total += (size_t)n;
    }
    if (memcmp(got, big, sizeof(big)) != 0)
        fail(8, "large data mismatch");

    /* Nonblocking read on an empty socket must return EAGAIN. */
    int fl = fcntl(sv[1], F_GETFL, 0);
    fcntl(sv[1], F_SETFL, fl | O_NONBLOCK);
    errno = 0;
    n = read(sv[1], got, 16);
    if (n != -1 || errno != EAGAIN)
        fail(9, "expected EAGAIN");
    fcntl(sv[1], F_SETFL, fl & ~O_NONBLOCK);

    /* Poll: POLLIN must fire once data arrives. */
    struct pollfd pfd = { .fd = sv[1], .events = POLLIN };
    if (poll(&pfd, 1, 0) != 0)
        fail(10, "poll should not report yet");
    if (write(sv[0], "x", 1) != 1)
        fail(11, "poll trigger write");
    int pr = poll(&pfd, 1, 2000);
    if (pr != 1 || !(pfd.revents & POLLIN))
        fail(12, "poll POLLIN missed");

    /* EOF: close the writer, the reader must see read() == 0. */
    close(sv[0]);
    char e;
    n = read(sv[1], &e, 1);
    if (n != 1) /* the pending 'x' */
        fail(13, "drain before EOF");
    n = read(sv[1], &e, 1);
    if (n != 0)
        fail(14, "expected EOF");

    close(sv[1]);
    return 0;
}

int main(void)
{
    if (roundtrip(SOCK_STREAM) != 0) return 1;
    if (roundtrip(SOCK_SEQPACKET) != 0) return 2;
    if (stream_large_and_partial() != 0) return 3;

    printf("UNIX_CH: PASS (channel-backed AF_UNIX socketpair)\n");
    return 0;
}
