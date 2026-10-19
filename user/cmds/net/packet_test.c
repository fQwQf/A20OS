/*
 * AF_PACKET raw L2 sockets: bind, getsockname, the option and readability
 * refusals, malformed send, and close/reuse.
 *
 * The point of this test is the paths the socket migration owns: the domain and
 * type checks in setsockopt, the AF_PACKET refusal in the FIONREAD gate, the
 * slot lifecycle across close and re-bind, and the packet-socket bind
 * validation itself.  It does not require traffic: the capture path needs a
 * frame to arrive, and asserting on one would make the gate depend on the
 * network topology rather than on the socket state machine.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

static int failures;
static int checks;

static void ok(int cond, const char *fmt, ...)
{
    checks++;
    if (!cond)
        failures++;
    printf("PACKET_TEST: %s ", cond ? "ok  " : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int main(void)
{
    /* if_nametoindex() is a libc call this image may not wire up; the test does
     * not depend on a named interface, only on a real AF_PACKET bind.  A
     * resolved name gets bound explicitly, otherwise index 0 means "any
     * interface" and the bind still exercises the same state machine. */
    unsigned lo = if_nametoindex("lo");
    int ifindex = lo > 0 ? (int)lo : 0;
    printf("PACKET_TEST: info if_nametoindex(\"lo\")=%u, binding ifindex=%d\n",
           lo, ifindex);

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    ok(fd >= 0, "socket(AF_PACKET, SOCK_RAW, ETH_P_ALL) succeeds (%d)", fd);
    if (fd < 0) {
        printf("PACKET_TEST: FAIL could not create a packet socket\n");
        return 1;
    }

    /* The migrated option machine refuses a socket family it cannot bound. */
    int rcvbuf = 4096;
    errno = 0;
    int rc = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    ok(rc < 0 && errno == EOPNOTSUPP,
       "SO_RCVBUF on AF_PACKET is refused with EOPNOTSUPP (rc=%d errno=%d)",
       rc, errno);

    int keepalive = 1;
    errno = 0;
    rc = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    ok(rc == 0, "SO_KEEPALIVE is accepted on AF_PACKET (rc=%d errno=%d)", rc,
       errno);

    int type = -1;
    socklen_t tlen = sizeof(type);
    ok(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0 &&
           type == SOCK_RAW,
       "getsockopt(SO_TYPE) reports SOCK_RAW (%d)", type);

    struct sockaddr_ll ll;
    memset(&ll, 0, sizeof(ll));
    ll.sll_family = AF_PACKET;
    ll.sll_protocol = htons(ETH_P_ALL);
    ll.sll_ifindex = ifindex;
    ok(bind(fd, (struct sockaddr *)&ll, sizeof(ll)) == 0,
       "bind to the loopback index succeeds (errno=%d)", errno);

    struct sockaddr_ll got;
    socklen_t glen = sizeof(got);
    memset(&got, 0, sizeof(got));
    ok(getsockname(fd, (struct sockaddr *)&got, &glen) == 0 &&
           got.sll_family == AF_PACKET && got.sll_ifindex == ifindex,
       "getsockname reports AF_PACKET on ifindex %d (%d)", got.sll_ifindex,
       got.sll_family);

    /* FIONREAD cannot be answered for a frame socket; the gate that says so is
     * the migrated one. */
    int pending = -1;
    errno = 0;
    rc = ioctl(fd, FIONREAD, &pending);
    ok(rc < 0 && errno == EOPNOTSUPP,
       "FIONREAD on AF_PACKET is refused with EOPNOTSUPP (rc=%d errno=%d)",
       rc, errno);

    /* A bind to an interface that does not exist must fail, not attach. */
    struct sockaddr_ll bad;
    memset(&bad, 0, sizeof(bad));
    bad.sll_family = AF_PACKET;
    bad.sll_protocol = htons(ETH_P_ALL);
    bad.sll_ifindex = 999;
    errno = 0;
    rc = bind(fd, (struct sockaddr *)&bad, sizeof(bad));
    ok(rc < 0 && errno == ENODEV,
       "bind to a missing interface fails with ENODEV (rc=%d errno=%d)", rc,
       errno);

    /* A wrong family in an L2 address is refused, not silently accepted. */
    bad.sll_family = AF_INET;
    bad.sll_ifindex = (int)lo;
    errno = 0;
    rc = bind(fd, (struct sockaddr *)&bad, sizeof(bad));
    ok(rc < 0 && (errno == EAFNOSUPPORT || errno == EINVAL),
       "bind with a non-AF_PACKET family is refused (rc=%d errno=%d)", rc,
       errno);

    /* Sending nothing has no frame to send. */
    errno = 0;
    rc = (int)send(fd, "", 0, 0);
    ok(rc < 0 && errno == EINVAL,
       "send with an empty buffer is refused with EINVAL (rc=%d errno=%d)", rc,
       errno);

    ok(close(fd) == 0, "close succeeds");

    /* Close/reuse: the slot and the census must be reusable immediately. */
    for (int i = 0; i < 4; i++) {
        fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        ok(fd >= 0, "reopen %d succeeds", i);
        if (fd < 0)
            break;
        memset(&ll, 0, sizeof(ll));
        ll.sll_family = AF_PACKET;
        ll.sll_protocol = htons(ETH_P_ALL);
        ll.sll_ifindex = ifindex;
        ok(bind(fd, (struct sockaddr *)&ll, sizeof(ll)) == 0,
           "rebind %d succeeds", i);
        ok(close(fd) == 0, "reclose %d succeeds", i);
    }

    /* SOCK_DGRAM is the header-stripped variant of the same socket. */
    fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
    ok(fd >= 0, "socket(AF_PACKET, SOCK_DGRAM) succeeds (%d)", fd);
    if (fd >= 0) {
        memset(&ll, 0, sizeof(ll));
        ll.sll_family = AF_PACKET;
        ll.sll_protocol = htons(ETH_P_ALL);
        ll.sll_ifindex = ifindex;
        ok(bind(fd, (struct sockaddr *)&ll, sizeof(ll)) == 0,
           "SOCK_DGRAM binds to the loopback index");
        close(fd);
    }

    if (failures) {
        printf("PACKET_TEST: FAIL (%d of %d checks failed)\n", failures, checks);
        return 1;
    }
    printf("PACKET_TEST: PASS (%d checks)\n", checks);
    return 0;
}
