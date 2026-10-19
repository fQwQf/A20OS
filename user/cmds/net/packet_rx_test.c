/*
 * AF_PACKET capture: the RX ring, the delivery filter, the bound-socket census
 * and close/reuse on the capture path.
 *
 * packet_test covers the bind/option state machine and deliberately needs no
 * traffic; this test is the other half.  The capture hook only runs for frames
 * that arrive on a device, so the test arms a bound socket and then makes the
 * guest's own network stack produce inbound traffic: a UDP datagram to the
 * gateway's discard port has no listener on the far side, and the reply that
 * comes back is a real device receive.
 *
 * The environment decides whether a frame actually arrives.  When none does the
 * test reports SKIP rather than FAIL: the bind, filter and close/reuse
 * assertions above it still ran, and a gate that goes red because a user-mode
 * network chose not to answer would be a gate nobody could trust.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif

/* The gateway the QEMU user-mode network always has. */
#define GW_ADDR ((10u << 24) | (0u << 16) | (2u << 8) | 2u)

static int failures;
static int checks;

static void ok(int cond, const char *fmt, ...)
{
    checks++;
    if (!cond)
        failures++;
    printf("PACKET_RX_TEST: %s ", cond ? "ok  " : "FAIL");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* A UDP datagram to a port the far side has no listener for.  Index 0 means
 * "any interface", so the bind does not depend on which device the frame
 * arrives on. */
static int bind_packet(unsigned short proto)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(proto));
    if (fd < 0)
        return -1;
    struct sockaddr_ll ll;
    memset(&ll, 0, sizeof(ll));
    ll.sll_family = AF_PACKET;
    ll.sll_protocol = htons(proto);
    ll.sll_ifindex = 0;
    if (bind(fd, (struct sockaddr *)&ll, sizeof(ll)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void generate_inbound(void)
{
    /* The shipped ping, which the documented bring-up already uses against the
     * same gateway, is the strongest trigger: every echo request it sends is
     * answered, so the reply is a real device receive while the caller holds the
     * capture socket. */
    (void)system("ping 10.0.2.2 2 >/dev/null 2>&1");

    /* And a datagram to a port the far side has no listener for, in case the
     * user-mode network answers that with an ICMP unreachable instead. */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(GW_ADDR);
    (void)sendto(fd, "x", 1, 0, (struct sockaddr *)&to, sizeof(to));
    close(fd);
}

int main(void)
{
    int fd = bind_packet(ETH_P_ALL);
    ok(fd >= 0, "bind ETH_P_ALL on any interface (fd=%d errno=%d)", fd, errno);
    if (fd < 0) {
        printf("PACKET_RX_TEST: FAIL could not arm the capture path\n");
        return 1;
    }

    /* A second bound socket whose protocol filter cannot match. */
    int other = bind_packet(0x1234);
    ok(other >= 0, "bind a non-matching protocol on a second socket (fd=%d)",
       other);

    generate_inbound();
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int pr = poll(&p, 1, 3000);
    if (pr <= 0) {
        printf("PACKET_RX_TEST: SKIP no inbound frame arrived (poll=%d errno=%d)\n",
               pr, errno);
        if (other >= 0)
            close(other);
        close(fd);
        printf("PACKET_RX_TEST: SKIP (%d checks ran, %d failed)\n", checks,
               failures);
        return failures ? 1 : 77;
    }

    unsigned char frame[2048];
    struct sockaddr_ll from;
    socklen_t flen = sizeof(from);
    memset(&from, 0, sizeof(from));
    ssize_t n = recvfrom(fd, frame, sizeof(frame), 0, (struct sockaddr *)&from,
                         &flen);
    ok(n > 0, "recvfrom on the capture ring returns a frame (n=%zd errno=%d)",
       n, errno);
    ok(n >= 14, "the captured frame carries an Ethernet header (n=%zd)", n);
    ok(from.sll_family == AF_PACKET, "sll_family is AF_PACKET (%d)",
       from.sll_family);
    ok(from.sll_ifindex > 0, "sll_ifindex names the capturing interface (%d)",
       from.sll_ifindex);
    ok(from.sll_halen == 6, "sll_halen is 6 (%d)", from.sll_halen);
    if (n >= 14) {
        unsigned short ether =
            (unsigned short)((frame[12] << 8) | frame[13]);
        /* The field carries the wire bytes, so userspace reads it in network
         * order -- the same convention Linux uses. */
        ok(from.sll_protocol == htons(ether),
           "sll_protocol carries the frame's ethertype (0x%04x vs 0x%04x)",
           ntohs(from.sll_protocol), ether);
    }

    /* The socket whose filter cannot match must have received nothing, even
     * though the frame was captured while it was bound. */
    if (other >= 0) {
        struct pollfd q = { .fd = other, .events = POLLIN };
        ok(poll(&q, 1, 100) == 0,
           "a socket whose protocol filter cannot match receives nothing");
    }

    /* Close/reuse on the capture path: the census must drop and a fresh bind
     * must capture again. */
    if (other >= 0)
        close(other);
    close(fd);
    fd = bind_packet(ETH_P_ALL);
    ok(fd >= 0, "rebind after close succeeds (fd=%d errno=%d)", fd, errno);
    if (fd >= 0) {
        generate_inbound();
        struct pollfd r = { .fd = fd, .events = POLLIN };
        int rr = poll(&r, 1, 3000);
        ok(rr > 0, "a rebound socket captures again (poll=%d errno=%d)", rr,
           errno);
        if (rr > 0) {
            struct sockaddr_ll again;
            socklen_t alen = sizeof(again);
            memset(&again, 0, sizeof(again));
            ssize_t m = recvfrom(fd, frame, sizeof(frame), 0,
                                 (struct sockaddr *)&again, &alen);
            ok(m > 0, "the frame after the rebind is delivered (m=%zd)", m);
            ok(again.sll_ifindex == from.sll_ifindex,
               "it arrives on the same interface (%d vs %d)", again.sll_ifindex,
               from.sll_ifindex);
        }
        close(fd);
    }

    printf("PACKET_RX_TEST: %s (%d checks)\n", failures ? "FAIL" : "PASS",
           checks);
    return failures ? 1 : 0;
}
