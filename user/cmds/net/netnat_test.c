/*
 * conntrack and NAT test.
 *
 * Run by tools/targets-smoke.mk:smoke-netfilter-nat, which launches QEMU with
 *
 *     hostfwd=tcp:127.0.0.1:18081-:10.0.2.15:18081
 *
 * and then, from the *host*, connects to 127.0.0.1:18081.  A connection
 * arriving at the guest therefore has destination 10.0.2.15:18081, while
 * nothing in the guest listens on 18081 -- this program listens on 18082.  The
 * only way the host's connection can reach that socket is a DNAT rewrite of
 * both the address and the port, so the accept() below is the assertion.
 *
 * What each part actually proves, and why the obvious cheaper check is not
 * used instead:
 *
 *   1. conntrack tracks a plain outbound flow with no NAT configured.  Counters
 *      only; nothing is listening at the destination.
 *   2. The NAT rule parser rejects rules that would match and then do nothing
 *      (dnat without to=, snat without to=, masquerade with to=, dnat on the
 *      output hook), and accepts a well-formed one.  A rule that silently does
 *      nothing is worse than no rule: its matched counter would report traffic
 *      being translated when none was.
 *   3. Negative control: connecting to 10.0.2.15:18081 from inside the guest is
 *      refused.  This is what makes step 4 attributable to the DNAT rule rather
 *      than to some other listener having appeared on the forwarded port.
 *   4. The end-to-end port forward: install the DNAT rule, accept the host's
 *      connection on 18082, and echo one byte back.  Echoing back also proves
 *      the reverse direction, because the host's QEMU will only accept a
 *      reply whose source port is 18081 again -- which is the conntrack entry's
 *      recorded DNAT tuple being applied outbound, not the rules being
 *      re-evaluated.
 *   5. The conntrack entry for that flow reports nat=dnat, so the binding was
 *      recorded rather than applied and forgotten.
 *
 * A wrong TCP checksum anywhere in that path -- the IP header's, or the
 * pseudo-header's -- means no SYN-ACK and no accept(), so the byte count of
 * this test is also the checksum fixup's assertion.  It is not a separate
 * check because it cannot be separated: there is no way to complete step 4
 * without it.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

/* Must match the hostfwd rule in tools/targets-smoke.mk:smoke-netfilter-nat. */
#define FWD_PORT   18081
#define LISTEN_PORT 18082

#define GUEST_IP   "10.0.2.15"
#define GATEWAY_IP "10.0.2.2"

#define NF_PATH "/proc/a20/netfilter"

#define CHK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("NETNAT_TEST: FAIL %s:%d %s (errno=%d %s)\n",               \
                   __func__, __LINE__, (msg), errno, strerror(errno));         \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int nf_read(char *buf, size_t bufsz)
{
    int fd = open(NF_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

static int nf_write(const char *s)
{
    int fd = open(NF_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = (n == (ssize_t)strlen(s)) ? 0 : -1;
    close(fd);
    return e;
}

static unsigned long long nf_stat(const char *text, const char *key)
{
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ')
                v++;
            return strtoull(v, NULL, 10);
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static int udp_send_once(const char *dst, unsigned port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(dst);
    ssize_t n = sendto(fd, "nat", 3, 0, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return n == 3 ? 0 : -1;
}

/* True when some `ct ` line mentions `needle`. */
static int ct_mentions(const char *text, const char *needle)
{
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, "ct ", 3) == 0 && strstr(p, needle))
            return 1;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

int main(void)
{
    char buf[4096];

    /* 1. conntrack, with no NAT configured at all. */
    CHK(nf_write("reset") == 0, "reset filter and NAT tables");
    CHK(nf_write("ctflush") == 0, "flush conntrack");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read netfilter node");
    CHK(strstr(buf, "conntrack: on") != NULL, "conntrack is on by default");
    CHK(nf_stat(buf, "ct_capacity") > 0, "a capacity is reported");
    CHK(nf_stat(buf, "ct_tracked") == 0, "table is empty after a flush");

    unsigned long long pkt0 = nf_stat(buf, "ct_packets");
    CHK(udp_send_once(GATEWAY_IP, 9999) == 0, "send one UDP datagram");
    CHK(udp_send_once(GATEWAY_IP, 9999) == 0, "send a second");

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after sends");
    unsigned long long tracked = nf_stat(buf, "ct_tracked");
    unsigned long long ctpkt = nf_stat(buf, "ct_packets");
    CHK(ctpkt > pkt0, "conntrack saw the datagrams");
    CHK(tracked >= 1, "the flow was tracked");
    CHK(strstr(buf, "nat=none") != NULL,
        "an untranslated flow reports nat=none rather than being omitted");

    /* 2. the NAT rule parser. */
    CHK(nf_write("natadd in proto=tcp dport=18081 action=dnat") < 0,
        "dnat without to= is rejected");
    CHK(nf_write("natadd out proto=tcp action=snat") < 0,
        "snat without to= is rejected");
    CHK(nf_write("natadd out proto=tcp action=masquerade to=10.0.2.15") < 0,
        "masquerade with to= is rejected: the interface supplies the address");
    CHK(nf_write("natadd out proto=tcp dport=1 action=dnat to=10.0.2.15") < 0,
        "dnat on the output hook is rejected");
    CHK(nf_write("natadd in proto=udp dport=1 action=masquerade toport=99") < 0,
        "masquerade with toport= is rejected");
    CHK(nf_write("natadd in action=drop to=10.0.2.15") < 0,
        "a filter action is not a NAT action");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after rejected rules");
    CHK(nf_stat(buf, "nat_rules") == 0, "no rejected rule was installed");

    CHK(nf_write("natadd out proto=udp action=masquerade") == 0,
        "a well-formed masquerade rule is accepted");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read back the NAT rule");
    CHK(nf_stat(buf, "nat_rules") == 1, "one NAT rule installed");
    CHK(strstr(buf, "action=masquerade") != NULL, "action read back");
    CHK(nf_write("natdel 0") == 0, "delete the NAT rule");
    CHK(nf_write("natdel 0") < 0, "out-of-range NAT delete rejected");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after NAT delete");
    CHK(nf_stat(buf, "nat_rules") == 0, "NAT table empty again");

    /*
     * 3. Negative control.  Nothing in this guest listens on the forwarded port,
     * and this proves it rather than assuming it -- if the forward below ever
     * succeeds for a reason other than the DNAT rule, this is the check that
     * says so.  The RST this provokes is an ICMP-free TCP error and passes
     * through the hooks without touching conntrack.
     */
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    CHK(probe >= 0, "create probe socket");
    struct sockaddr_in pa;
    memset(&pa, 0, sizeof(pa));
    pa.sin_family = AF_INET;
    pa.sin_port = htons(FWD_PORT);
    pa.sin_addr.s_addr = inet_addr(GUEST_IP);
    int prc = connect(probe, (struct sockaddr *)&pa, sizeof(pa));
    close(probe);
    CHK(prc < 0, "nothing in the guest listens on the forwarded port");

    /* 4. The forward itself. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHK(lfd >= 0, "create listener");
    int one = 1;
    CHK(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0,
        "SO_REUSEADDR");
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = htons(LISTEN_PORT);
    la.sin_addr.s_addr = inet_addr(GUEST_IP);
    CHK(bind(lfd, (struct sockaddr *)&la, sizeof(la)) == 0, "bind listener");
    CHK(listen(lfd, 4) == 0, "listen");
    printf("NETNAT_TEST: listening on %s:%d, forwarding %d\n", GUEST_IP,
           LISTEN_PORT, FWD_PORT);

    char rule[128];
    snprintf(rule, sizeof(rule),
             "natadd in proto=tcp dport=%d action=dnat to=%s toport=%d",
             FWD_PORT, GUEST_IP, LISTEN_PORT);
    CHK(nf_write(rule) == 0, "install the DNAT rule");

    /*
     * Bounded wait, and the counters are the point of the bound.  An unbounded
     * accept() reports a hang only as "the smoke recipe saw no verdict", which
     * says nothing about *where* it stopped: a hook that never fired, a rewrite
     * that reached lwIP with a bad checksum, and a conntrack lookup that
     * swallowed the SYN all look identical from the outside.  Dumping the node
     * on timeout separates them -- in_packets/in_accepted say whether the input
     * hook ran at all, and a ct line with the host's tuple says whether the
     * rewrite happened and the packet went on to a socket.
     */
    CHK(nf_read(buf, sizeof(buf)) > 0, "read before waiting");
    unsigned long long in0 = nf_stat(buf, "in_packets");

    printf("NETNAT_TEST: waiting for the host connection\n");
    struct pollfd pfd = { .fd = lfd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, 30000);
    if (pr <= 0) {
        if (nf_read(buf, sizeof(buf)) > 0) {
            unsigned long long in1 = nf_stat(buf, "in_packets");
            printf("NETNAT_TEST: no connection after %d ms "
                   "(in_packets +%llu)\n", pr == 0 ? 30000 : -1, in1 - in0);
            printf("%s", buf);
        }
        printf("NETNAT_TEST: FAIL main: the forwarded connection never arrived\n");
        return 1;
    }

    int cfd = accept(lfd, NULL, NULL);
    CHK(cfd >= 0, "accept the DNAT'd connection");

    ssize_t w = write(cfd, "A", 1);
    CHK(w == 1, "write the echo byte");
    char rb = 0;
    ssize_t r = read(cfd, &rb, 1);
    CHK(r == 1 && rb == 'A', "the byte came back through the reverse rewrite");
    close(cfd);
    close(lfd);

    /* 5. the binding was recorded, not just applied. */
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after the forward");
    CHK(strstr(buf, "nat=dnat") != NULL,
        "the conntrack entry records the DNAT binding");
    CHK(ct_mentions(buf, "state=established") ||
        ct_mentions(buf, "state=new"),
        "the forwarded flow has a conntrack entry with a state");
    CHK(nf_stat(buf, "ct_tracked") >= 1, "the table is not empty");

    printf("NETNAT_TEST: PASS dnat %d -> %s:%d\n", FWD_PORT, GUEST_IP,
           LISTEN_PORT);
    return 0;
}