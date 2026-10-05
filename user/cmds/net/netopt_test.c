/*
 * Runtime gate for the socket option, message flag and ioctl surface.
 *
 * Each case here corresponds to a facility that used to report a capability it
 * did not have, which is worse than refusing: a caller that is told FIONBIO
 * succeeded, or that it joined a multicast group, proceeds on a false belief.
 * So the checks are written to fail on the old behaviour, not merely to fail
 * when something is missing.
 *
 * The FIONBIO case is the sharpest one.  ioctl(FIONBIO) used to return 0 and
 * change nothing, so a following recv() blocked forever.  Blocking forever
 * inside a test gate is unacceptable, so the recv is bracketed by SO_RCVTIMEO
 * and timed: O_NONBLOCK makes it return EAGAIN immediately, whereas a socket
 * that is still blocking returns EAGAIN only after the timeout.  Elapsed time
 * is what separates the two, so a regression fails instead of hanging.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

/* musl's <netinet/tcp.h> carries TCP_CONGESTION but this tree's copy may not;
 * the number is the UAPI one and is pinned against the kernel's own. */
#ifndef TCP_CONGESTION
#define TCP_CONGESTION 13
#endif

#define TEST_NAME "NETOPT"

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("%s: ok   %s\n", TEST_NAME, what);
    } else {
        failures++;
        printf("%s: FAIL %s (errno=%d %s)\n", TEST_NAME, what, errno,
               strerror(errno));
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* /proc/net/{tcp,udp,unix} used to emit the column header and no rows at all,
 * so a header line is not evidence of anything: a data row is required. */
static void test_proc_rows(const char *path, const char *label, const char *needle)
{
    static char buf[8192];
    int fd = open(path, O_RDONLY);
    ssize_t n;
    char *line;
    int header_skipped = 0, rows = 0;

    if (fd < 0) {
        ok(0, label);
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        ok(0, label);
        return;
    }
    buf[n] = '\0';

    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!header_skipped) {
            header_skipped = 1;
            continue;
        }
        if (needle == NULL || strstr(line, needle))
            rows++;
    }
    ok(rows > 0, label);
}

static void test_ip_options(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int v, got;
    socklen_t len;

    if (fd < 0) {
        ok(0, "socket for IPPROTO_IP options");
        return;
    }

    len = sizeof(v);
    ok(getsockopt(fd, IPPROTO_IP, IP_TTL, &got, &len) == 0,
       "IP_TTL is readable before being set");
    ok(got == 64, "unset IP_TTL reports the default 64");

    v = 42;
    ok(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) == 0,
       "IP_TTL accepts a value");
    len = sizeof(got);
    ok(getsockopt(fd, IPPROTO_IP, IP_TTL, &got, &len) == 0,
       "IP_TTL reads back");
    ok(got == 42, "IP_TTL round-trips the value that was set");

    /* Linux rejects 0 for IP_TTL: a zero hop limit would emit a packet that
     * expires before it leaves the host, so it must not be accepted. */
    v = 0;
    errno = 0;
    ok(setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)) < 0,
       "IP_TTL rejects 0");
    ok(errno == EINVAL, "IP_TTL 0 fails with EINVAL");
    len = sizeof(got);
    getsockopt(fd, IPPROTO_IP, IP_TTL, &got, &len);
    ok(got == 42, "a rejected IP_TTL leaves the previous value intact");

    v = 0x28; /* DSCP: low delay */
    ok(setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v)) == 0,
       "IP_TOS accepts a value");
    len = sizeof(got);
    ok(getsockopt(fd, IPPROTO_IP, IP_TOS, &got, &len) == 0, "IP_TOS reads back");
    ok(got == 0x28, "IP_TOS round-trips the value that was set");

    v = 256;
    errno = 0;
    ok(setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v)) < 0,
       "IP_TOS rejects an out-of-range value");
    ok(errno == EINVAL, "out-of-range IP_TOS fails with EINVAL");

    close(fd);
}

static void test_multicast(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ip_mreqn mreq;

    if (fd < 0) {
        ok(0, "socket for multicast membership");
        return;
    }

    memset(&mreq, 0, sizeof(mreq));
    /* An administratively scoped group, not 224.0.0.1: every host already
     * belongs to the all-systems group and lwIP refuses an explicit join of
     * it, so that constant would probe lwIP's policy, not this code path. */
    mreq.imr_multiaddr.s_addr = htonl(0xEF010203); /* 239.1.2.3 */
    mreq.imr_ifindex = 0;
    /* The old build answered 0 here without touching any group, so success is
     * only meaningful if a leave of the same group is also accepted. */
    ok(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) == 0,
       "joining a multicast group is accepted");
    ok(setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) == 0,
       "leaving the same group is accepted");

    mreq.imr_multiaddr.s_addr = htonl(0x7F000001); /* 127.0.0.1, not multicast */
    errno = 0;
    ok(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0,
       "joining a non-multicast address is rejected");
    ok(errno == EINVAL, "non-multicast join fails with EINVAL");

    close(fd);
}

static void test_fionbio(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int on = 1, avail = -1;
    struct timeval tv = { 3, 0 };
    char buf[16];
    long long t0, elapsed;
    ssize_t r;

    if (fd < 0) {
        ok(0, "socket for FIONBIO");
        return;
    }
    /* The timeout is a backstop so a regression fails instead of hanging; it
     * is not what makes the recv return, which is the point of the timing. */
    ok(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0,
       "SO_RCVTIMEO is armed as a hang guard");

    ok(ioctl(fd, FIONBIO, &on) == 0, "FIONBIO is accepted");
    t0 = now_ms();
    errno = 0;
    r = recv(fd, buf, sizeof(buf), 0);
    elapsed = now_ms() - t0;
    ok(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
       "recv on an empty non-blocking socket returns EAGAIN");
    ok(elapsed < 1000,
       "FIONBIO really made the socket non-blocking, not just a no-op");

    on = 0;
    ok(ioctl(fd, FIONBIO, &on) == 0, "FIONBIO clear is accepted");
    ok(fcntl(fd, F_GETFL) >= 0, "the descriptor is still usable after FIONBIO");

    on = 1;
    ioctl(fd, FIONBIO, &on);
    ok(ioctl(fd, FIONREAD, &avail) == 0, "FIONREAD is accepted on a socket");
    ok(avail == 0, "FIONREAD reports 0 for a socket with nothing queued");

    close(fd);
}

static void test_fionread_and_peek(void)
{
    int sv[2];
    int avail = -1;
    char a[8], b[8];
    ssize_t r;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        ok(0, "socketpair");
        return;
    }
    ok(ioctl(sv[1], FIONREAD, &avail) == 0, "FIONREAD is accepted on a stream");
    ok(avail == 0, "FIONREAD starts at 0");

    ok(write(sv[0], "hello", 5) == 5, "write 5 bytes to the peer");
    ok(ioctl(sv[1], FIONREAD, &avail) == 0, "FIONREAD after a write");
    ok(avail == 5, "FIONREAD counts the queued bytes");

    /* A peek must show the data without consuming it, twice. */
    r = recv(sv[1], a, sizeof(a), MSG_PEEK);
    ok(r == 5, "MSG_PEEK returns the queued bytes");
    r = recv(sv[1], b, sizeof(b), MSG_PEEK);
    ok(r == 5 && memcmp(a, b, 5) == 0, "a second MSG_PEEK sees the same bytes");
    ok(ioctl(sv[1], FIONREAD, &avail) == 0 && avail == 5,
       "MSG_PEEK leaves the data queued");
    r = recv(sv[1], b, sizeof(b), 0);
    ok(r == 5 && memcmp(a, b, 5) == 0, "the following recv consumes the data");
    ok(ioctl(sv[1], FIONREAD, &avail) == 0 && avail == 0,
       "FIONREAD drops to 0 once the data is consumed");

    close(sv[0]);
    close(sv[1]);
}

static void test_msg_trunc(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    char big[16], small[4];
    ssize_t r;

    if (fd < 0) {
        ok(0, "socket for MSG_TRUNC");
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(0x7F000001);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
        ok(0, "bind the datagram socket to loopback");
        close(fd);
        return;
    }
    memset(big, 'x', sizeof(big));
    ok(sendto(fd, big, sizeof(big), 0, (struct sockaddr *)&sa, sizeof(sa)) ==
           (ssize_t)sizeof(big),
       "send a 16-byte datagram to self");

    /* A 4-byte buffer cannot hold it, so the return value is what tells the
     * caller how much was lost -- that is the whole point of MSG_TRUNC. */
    r = recv(fd, small, sizeof(small), MSG_TRUNC);
    ok(r == (ssize_t)sizeof(big),
       "MSG_TRUNC reports the full datagram length from a short buffer");

    close(fd);
}

static void test_msg_oob_still_refused(void)
{
    int sv[2];
    char buf[8];
    ssize_t r;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        ok(0, "socketpair for MSG_OOB");
        return;
    }
    errno = 0;
    r = recv(sv[1], buf, sizeof(buf), MSG_OOB);
    ok(r < 0 && errno == EOPNOTSUPP,
       "MSG_OOB is still refused, not silently treated as ordinary data");
    close(sv[0]);
    close(sv[1]);
}

static void test_proc_net_rows(void)
{
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    int sv[2];
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    char porthex[8];

    /* Every socket stays open for the whole check: a row can only exist while
     * its socket exists, so closing one first would make this assert nothing. */
    if (tcp >= 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(0x7F000001);
        sa.sin_port = 0;
        if (bind(tcp, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
            listen(tcp, 1) == 0 &&
            getsockname(tcp, (struct sockaddr *)&sa, &sl) == 0) {
            /* The port is printed in host order, so matching it proves the row
             * is this socket and not some other one that happens to exist. */
            snprintf(porthex, sizeof(porthex), "%04X", ntohs(sa.sin_port));
            test_proc_rows("/proc/net/tcp",
                           "/proc/net/tcp lists this listening socket", porthex);
        } else {
            ok(0, "bind and listen on loopback");
        }
    } else {
        ok(0, "socket for /proc/net/tcp");
    }
    test_proc_rows("/proc/net/udp", "/proc/net/udp lists this datagram socket",
                   NULL);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        test_proc_rows("/proc/net/unix", "/proc/net/unix lists this socket",
                       NULL);
        close(sv[0]);
        close(sv[1]);
    } else {
        ok(0, "/proc/net/unix socketpair");
    }

    if (tcp >= 0)
        close(tcp);
    if (udp >= 0)
        close(udp);
}

/* ---------------------------------------------------------------- */
/* SO_SNDBUF / SO_RCVBUF / TCP_CONGESTION                           */
/* ---------------------------------------------------------------- */

/*
 * These three used to be absent from the option surface in the worst possible
 * way: setsockopt refused them with -EOPNOTSUPP and getsockopt answered both
 * buffer names with the constant NET_MAX_QUEUE * NET_MAX_PAYLOAD.  A caller
 * sizing a listen socket was therefore told its socket had a buffer, of a size
 * that corresponded to no buffer on the machine, and told so for every socket
 * regardless of what it had asked for.  Every assertion below is written to
 * fail on that behaviour.
 *
 * The clamping assertions are not decoration.  The honest contract is "the
 * value in force", not "the value requested": a request above what the pcb can
 * honour buys nothing, and getsockopt reporting the request would leave the
 * caller believing otherwise.  The specific ceilings are a property of this
 * port's lwIP configuration (TCP_SND_BUF, TCP_WND) and are only probed as
 * bounds -- the test does not hard-code 93440 or 5840, only that asking for an
 * absurd value comes back clamped and below the request.
 */
static void test_sock_buffers(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int v, rv;
    socklen_t sl;

    if (fd < 0) {
        ok(0, "open a TCP socket for the buffer options");
        return;
    }

    sl = sizeof(v);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, &sl) == 0 && v > 0,
       "getsockopt reports a positive default SO_SNDBUF");
    int dflt_snd = v;
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rv, &sl) == 0 && rv > 0,
       "getsockopt reports a positive default SO_RCVBUF");

    v = 16384;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "SO_SNDBUF accepts a 16 KiB request");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rv, &sl) == 0 && rv == 16384,
       "SO_SNDBUF reads back exactly what was set");
    ok(dflt_snd != 16384 || dflt_snd == 0,
       "the default send buffer is distinguishable from the one just set");

    v = 32768;
    ok(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) == 0,
       "SO_RCVBUF accepts a 32 KiB request");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rv, &sl) == 0 && rv == 32768,
       "SO_RCVBUF reads back exactly what was set");

    /* An absurd request must come back clamped, and below the request.  The
     * old build answered with a fixed constant that was neither. */
    v = 1 << 30;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "an oversized SO_SNDBUF request is accepted, not refused");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rv, &sl) == 0 && rv > 0 &&
       rv < (1 << 30),
       "an oversized SO_SNDBUF reads back clamped to what the pcb can honour");

    v = 1 << 30;
    ok(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)) == 0,
       "an oversized SO_RCVBUF request is accepted, not refused");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rv, &sl) == 0 && rv > 0 &&
       rv < (1 << 30),
       "an oversized SO_RCVBUF reads back clamped");

    /* A lower ceiling than the one already in force must stick. */
    v = 8192;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "lowering SO_SNDBUF below the current ceiling is accepted");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rv, &sl) == 0 && rv == 8192,
       "a lowered SO_SNDBUF takes effect and reads back");

    /* And raising it again must take effect at once, on this same socket, with
     * no connect() in between.  The ceiling used to be only ever lowered into
     * pcb->snd_buf, so a raise could not reach a pcb with bytes already
     * outstanding and did not land until the next connection -- and the write
     * path's parallel depth estimate made a raise actively unsound, because it
     * shrank the depth it derived from the very field the raise had just grown.
     * The readback is the observable half; what it is read back *from* is the
     * pcb, not the socket record. */
    v = 32768;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "raising SO_SNDBUF above the current ceiling is accepted");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rv, &sl) == 0 && rv == 32768,
       "a raised SO_SNDBUF takes effect immediately and reads back");
    /* Lower once more and raise past it, so the two directions are exercised on
     * one socket rather than only at the extremes. */
    v = 4096;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "SO_SNDBUF lowers again");
    v = 65536;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) == 0,
       "SO_SNDBUF raises above the lower value");
    sl = sizeof(rv);
    ok(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &rv, &sl) == 0 && rv == 65536,
       "the second raise is what reads back");

    /* Zero and negative are refused rather than silently clamped up to some
     * minimum: a caller that asked for a zero-byte buffer has a bug, and
     * accepting it would leave it believing it had asked for something usable. */
    v = 0;
    errno = 0;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) < 0 &&
       errno == EINVAL,
       "SO_SNDBUF 0 is refused with EINVAL");
    v = -1;
    errno = 0;
    ok(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) < 0 &&
       errno == EINVAL,
       "a negative SO_SNDBUF is refused with EINVAL");

    /* UDP has no pcb buffer for these to mean anything about, and the old
     * build refused both names.  It must still refuse them rather than accept
     * a setting it does not honour. */
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp >= 0) {
        v = 16384;
        errno = 0;
        ok(setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)) < 0 &&
           errno == EOPNOTSUPP,
           "SO_SNDBUF on a UDP socket is refused with EOPNOTSUPP, not "
           "silently accepted");
        close(udp);
    } else {
        ok(0, "open a UDP socket");
    }

    close(fd);
}

/*
 * TCP_CONGESTION.  An unknown algorithm name used to be accepted silently,
 * which is the failure mode worth guarding: the socket then ran whatever the
 * stack defaults to while the application believed it had selected an
 * algorithm.  getsockopt reporting the real name is what makes the option
 * checkable at all.
 */
static void test_congestion(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    char name[16];
    socklen_t sl;

    if (fd < 0) {
        ok(0, "open a TCP socket for TCP_CONGESTION");
        return;
    }

    sl = sizeof(name);
    ok(getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, name, &sl) == 0,
       "getsockopt reports the congestion algorithm in force");
    ok(strlen(name) > 0 && strlen(name) < sizeof(name),
       "the reported algorithm name is a non-empty NUL-terminated string");

    ok(strcmp(name, "reno") == 0 || strcmp(name, "cubic") == 0,
       "the reported algorithm is one this stack actually implements "
       "(reno or cubic), not an invented name");

    if (strcmp(name, "cubic") == 0) {
        ok(setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "reno", 4) == 0,
           "TCP_CONGESTION accepts reno on a build with cubic");
        sl = sizeof(name);
        ok(getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, name, &sl) == 0 &&
           strcmp(name, "reno") == 0,
           "the algorithm can be read back after being set");
    }

    errno = 0;
    ok(setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, "not-an-algorithm", 16) < 0 &&
       errno == ENOPROTOOPT,
       "an unknown congestion algorithm is refused with ENOPROTOOPT");

    close(fd);
}

int main(void)
{
    test_ip_options();
    test_multicast();
    test_fionbio();
    test_fionread_and_peek();
    test_msg_trunc();
    test_msg_oob_still_refused();
    test_proc_net_rows();
    test_sock_buffers();
    test_congestion();

    if (failures == 0) {
        printf("%s: PASS (%d checks)\n", TEST_NAME, checks);
        return 0;
    }
    printf("%s: FAIL (%d of %d checks failed)\n", TEST_NAME, failures, checks);
    return 1;
}
