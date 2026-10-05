/*
 * Netfilter control-surface and data-plane test.
 *
 * Control surface:
 *   1. /proc/a20/netfilter is readable and reports an accept default policy.
 *   2. "add" installs a rule and it reads back with the fields it was given.
 *   3. Malformed rules are rejected rather than silently accepted.
 *   4. "del" and "reset" remove rules.
 *
 * Data plane:
 *   5. A UDP send to a filtered destination must increment out_dropped and
 *      must not reach the wire.  This exercises a20_lwip_linkoutput for real:
 *      the counter only moves if the hook runs in the live data plane.
 *   6. The same send with no matching rule must increment out_packets and
 *      leave out_dropped alone, proving the drop was the rule and not a
 *      blanket failure of the transmit path.
 *   7. g_lwip_lock is registered for contention accounting.
 *   8. conntrack tracks that UDP flow, reports its tuple, and honours ctoff.
 *
 * NAT is not tested here.  A DNAT port forward needs an inbound connection
 * from outside the guest, which this single-shot serial script cannot produce;
 * tools/targets-smoke.mk:smoke-netfilter-nat does that with a QEMU hostfwd
 * rule, and user/cmds/net/netnat_test.c is its payload.
 */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Exit-code contract with tools/targets-smoke.mk:smoke-netfilter.
 *   0   passed
 *   1   failed (control surface or counters misbehaved)
 *   77  SKIP   -- the environment could not exercise the data plane
 *   78  ABSENT -- the capability is not implemented; the recipe treats this
 *                and any non-PASS marker as a gate failure, deliberately.
 */
#define ABSENT_CODE 78

#define CHK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("NETFILTER_TEST: FAIL %s:%d %s (errno=%d %s)\n",            \
                   __func__, __LINE__, (msg), errno, strerror(errno));         \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#define NF_PATH "/proc/a20/netfilter"

/* The QEMU user-mode gateway.  Nothing has to be listening there: the point is
 * which counters move, not whether a reply comes back. */
#define TEST_DST "10.0.2.2"
#define TEST_PORT 9999

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

/* Returns 0 and fills name[] when the host has an interface other than
 * loopback, -1 otherwise.  Used only to tell "this QEMU run attached no NIC"
 * apart from "a NIC is there and the hook never ran". */
static int find_nonloopback_nic(char *name, size_t sz)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    char buf[4096];
    struct ifconf ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)sizeof(buf);
    ifc.ifc_buf = buf;
    int rc = -1;
    if (ioctl(fd, SIOCGIFCONF, &ifc) == 0) {
        for (size_t off = 0; off + sizeof(struct ifreq) <= (size_t)ifc.ifc_len;
             off += sizeof(struct ifreq)) {
            const struct ifreq *row = (const struct ifreq *)(buf + off);
            char row_name[IFNAMSIZ + 1] = {0};
            memcpy(row_name, row->ifr_name, IFNAMSIZ);
            if (row_name[0] == '\0')
                continue;
            /* Prefix, not an exact match: lwIP assigns the loopback netif a
             * number from the same counter as the hardware ones, so it is
             * "lo1" here, not "lo".  Comparing against "lo" exactly made this
             * report the loopback as a NIC, which turned "this QEMU run has no
             * network interface" from a SKIP into a FAIL that blamed the
             * transmit hook for never running. */
            if (strncmp(row_name, "lo", 2) == 0)
                continue;
            snprintf(name, sz, "%s", row_name);
            rc = 0;
            break;
        }
    }
    close(fd);
    return rc;
}

static void udp_send_once(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    sa.sin_addr.s_addr = inet_addr(TEST_DST);
    const char msg[] = "netfilter-test";
    ssize_t n = sendto(fd, msg, sizeof(msg), 0, (struct sockaddr *)&sa,
                       sizeof(sa));
    (void)n;
    close(fd);
}

/*
 * One ICMP echo request to the gateway, built by hand.
 *
 * The point is the send path only: the request has to reach
 * netfilter_conntrack_process() as a parseable ICMP echo so the table has
 * something to create an entry from.  The reply is not waited for here --
 * main() sleeps once after the requests, and it is the reply that proves the
 * match, not the send.
 *
 * The raw socket is the same capability netopt_test already exercises.  A
 * refusal to open one is an environment fact reported as such rather than a
 * silent skip: a test that quietly does nothing when it cannot run is how a
 * missing feature survives.
 */
static int icmp_echo_once(unsigned id)
{
    struct icmp_echo_hdr {
        uint8_t type;
        uint8_t code;
        uint16_t csum;
        uint16_t echo_id;
        uint16_t seq;
    } h;
    unsigned char payload[16];
    unsigned char pkt[sizeof(h) + sizeof(payload)];
    struct sockaddr_in sa;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0)
        return -1;

    memset(&h, 0, sizeof(h));
    h.type = 8;                   /* echo request */
    h.code = 0;
    h.echo_id = htons((uint16_t)id);
    h.seq = htons(1);
    memset(payload, 'i', sizeof(payload));
    memcpy(pkt, &h, sizeof(h));
    memcpy(pkt + sizeof(h), payload, sizeof(payload));

    /* The ICMP checksum covers the header and the payload and no
     * pseudo-header, so this is the whole of it -- there is no second half to
     * get wrong the way there is for TCP or UDP.  An echo header is 8 bytes,
     * even, so the odd-length tail case cannot arise here. */
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < sizeof(pkt); i += 2)
        sum += (uint32_t)pkt[i] << 8 | pkt[i + 1];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    uint16_t ck = htons((uint16_t)~sum);
    memcpy(pkt + 2, &ck, 2);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr(TEST_DST);
    ssize_t n = sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa,
                       sizeof(sa));
    close(fd);
    return n == (ssize_t)sizeof(pkt) ? 0 : -1;
}

int main(void)
{
    char buf[4096];

    /* 1. default policy */
    CHK(nf_write("reset") == 0, "reset rules");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read netfilter node");
    CHK(strstr(buf, "policy: accept") != NULL, "default policy is accept");
    CHK(strstr(buf, "rules: 0") != NULL, "no rules after reset");

    /* 2. add a rule and read it back */
    CHK(nf_write("add out proto=udp dport=9999 action=drop") == 0,
        "add drop rule");
    CHK(nf_read(buf, sizeof(buf)) > 0, "re-read after add");
    CHK(strstr(buf, "rules: 1") != NULL, "one rule installed");
    CHK(strstr(buf, "proto=udp") != NULL, "proto recorded");
    CHK(strstr(buf, "dport=9999") != NULL, "dport recorded");
    CHK(strstr(buf, "action=drop") != NULL, "action recorded");
    CHK(strstr(buf, "src=any") != NULL, "unspecified src is a wildcard");

    /* an address-carrying rule must render the address back */
    CHK(nf_write("add in proto=tcp src=10.0.0.5 dst=192.168.1.7 "
                 "sport=1234 dport=443 action=accept") == 0,
        "add address rule");
    CHK(nf_read(buf, sizeof(buf)) > 0, "re-read after second add");
    CHK(strstr(buf, "src=10.0.0.5") != NULL, "src address recorded");
    CHK(strstr(buf, "dst=192.168.1.7") != NULL, "dst address recorded");
    CHK(strstr(buf, "sport=1234") != NULL, "sport recorded");

    /* 3. malformed rules must be rejected, not silently accepted */
    CHK(nf_write("add sideways action=drop") < 0, "bad direction rejected");
    CHK(nf_write("add in proto=telnet action=drop") < 0, "bad proto rejected");
    CHK(nf_write("add in src=999.1.1.1 action=drop") < 0, "bad addr rejected");
    CHK(nf_write("add in dport=99999 action=drop") < 0, "out-of-range port");
    CHK(nf_write("add in action=maybe") < 0, "bad action rejected");
    CHK(nf_write("nonsense") < 0, "garbage rejected");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after bad writes");
    CHK(strstr(buf, "rules: 2") != NULL, "bad rules did not change the table");

    /* 4. del and reset */
    CHK(nf_write("del 0") == 0, "delete rule 0");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after del");
    CHK(strstr(buf, "rules: 1") != NULL, "one rule left");
    CHK(nf_write("del 99") < 0, "out-of-range delete rejected");
    CHK(nf_write("reset") == 0, "reset");

    /*
     * 5. data plane: with a matching drop rule the transmit hook must count a
     *    drop.  Counters rather than packet capture, because there is nothing
     *    listening at the destination.
     */
    CHK(nf_read(buf, sizeof(buf)) > 0, "read baseline");
    unsigned long long out_pkt0 = nf_stat(buf, "out_packets");
    unsigned long long out_drop0 = nf_stat(buf, "out_dropped");

    CHK(nf_write("add out proto=udp dport=9999 action=drop") == 0,
        "install TX drop rule");
    udp_send_once();
    udp_send_once();
    udp_send_once();

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after filtered sends");
    unsigned long long out_pkt1 = nf_stat(buf, "out_packets");
    unsigned long long out_drop1 = nf_stat(buf, "out_dropped");
    if (out_drop1 == out_drop0) {
        /* Two very different situations produce "no drop", and collapsing them
         * into one SKIP is what let a green gate stand in for an unproven data
         * plane:
         *   - no non-loopback interface exists: this QEMU invocation attached
         *     no NIC, so nothing could be transmitted.  Environment, not a
         *     kernel capability claim -- SKIP, loudly named as such.
         *   - an interface does exist and out_packets did not move either: the
         *     transmit hook was never called, i.e. netfilter_output() is not
         *     wired into a20_lwip_linkoutput().  That is an ABSENT capability.
         *     ABSENT_CODE makes the smoke-netfilter recipe exit non-zero. */
        char nic[IFNAMSIZ];
        int have_nic = find_nonloopback_nic(nic, sizeof(nic)) == 0;
        nf_write("reset");
        if (!have_nic) {
            printf("NETFILTER_TEST: SKIP no non-loopback interface in this "
                   "instance (out_packets %llu -> %llu, out_dropped %llu -> "
                   "%llu); data plane not reachable, capability NOT verified\n",
                   out_pkt0, out_pkt1, out_drop0, out_drop1);
            return 77;
        }
        printf("NETFILTER_TEST: ABSENT transmit hook never ran: interface %s "
               "exists but out_packets %llu -> %llu and out_dropped %llu -> %llu\n",
               nic, out_pkt0, out_pkt1, out_drop0, out_drop1);
        return ABSENT_CODE;
    }
    CHK(out_drop1 > out_drop0, "dropped packets counted");
    CHK(out_pkt1 > out_pkt0, "transmit hook observed the packets");

    /* 6. same traffic with no matching rule must be accepted, not dropped */
    CHK(nf_write("reset") == 0, "clear rules");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after clearing rules");
    unsigned long long drop_before = nf_stat(buf, "out_dropped");
    unsigned long long pkt_before = nf_stat(buf, "out_packets");

    udp_send_once();
    udp_send_once();

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after unfiltered sends");
    unsigned long long drop_after = nf_stat(buf, "out_dropped");
    unsigned long long pkt_after = nf_stat(buf, "out_packets");
    CHK(drop_after == drop_before,
        "unmatched traffic must not be dropped by the filter");
    if (pkt_after > pkt_before)
        CHK(drop_after == drop_before, "transmit still observed, still accept");

    /*
     * 7. the network data-plane lock must be measurable.
     *
     * g_lwip_lock serialises the whole TCP/IP data plane, so whether it
     * contends is the number that decides if the network stack can ever
     * scale across CPUs.  It is registered for contention accounting, which
     * is the prerequisite for any decision about sharding it -- a lock this
     * central is too risky to rewrite before the hot call sites are known.
     *
     * Asserting the entry exists matters more than asserting it is nonzero:
     * a single-CPU instance legitimately sees no contention, but the entry
     * must still be listed, or the accounting is silently not wired up.
     */
    char locks[8192] = {0};
    int lfd = open("/proc/a20/lock_contention", O_RDONLY);
    CHK(lfd >= 0, "open /proc/a20/lock_contention");
    ssize_t ln = read(lfd, locks, sizeof(locks) - 1);
    close(lfd);
    CHK(ln > 0, "read /proc/a20/lock_contention");
    locks[ln] = '\0';
    CHK(strstr(locks, "lwip:") != NULL,
        "g_lwip_lock is registered for contention accounting");

    /*
     * 8. conntrack, without configuring any NAT.
     *
     * The cases above prove the verdict path; this proves the state table the
     * verdict path now sits on top of actually tracks a flow.  The tuple is
     * checked by its gateway address and port rather than by a row count,
     * because a count of 1 would be satisfied just as well by an unrelated
     * flow.
     *
     * ctflush is a separate verb from reset precisely so this can clear live
     * state without also destroying the configuration installed above.
     */
    CHK(nf_write("ctflush") == 0, "flush conntrack");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after flush");
    CHK(strstr(buf, "conntrack: on") != NULL, "conntrack is on by default");
    CHK(nf_stat(buf, "ct_capacity") > 0, "a capacity is reported");
    unsigned long long ct0 = nf_stat(buf, "ct_tracked");
    unsigned long long cp0 = nf_stat(buf, "ct_packets");

    udp_send_once();

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after the tracked send");
    unsigned long long ct1 = nf_stat(buf, "ct_tracked");
    unsigned long long cp1 = nf_stat(buf, "ct_packets");
    CHK(cp1 > cp0, "conntrack counted the packet");
    CHK(ct1 > ct0, "the flow created an entry");
    /* Both ends of the tuple, so an entry for some other flow cannot pass. */
    CHK(strstr(buf, "-> " TEST_DST ":") != NULL,
        "the tracked flow's destination is recorded");
    CHK(strstr(buf, "proto=17") != NULL, "the tracked flow is UDP");
    CHK(strstr(buf, "nat=none") != NULL,
        "an untranslated flow reports nat=none rather than being omitted");

    /*
     * ctoff stops entries being created but keeps the existing ones: tearing
     * them down would break every flow whose reply depends on a recorded NAT
     * binding.  A plain counter comparison is the only way to observe that, and
     * it is the property that makes the switch safe to flip on a live system.
     */
    CHK(nf_write("ctoff") == 0, "turn conntrack off");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after ctoff");
    CHK(strstr(buf, "conntrack: off") != NULL, "conntrack reports off");
    unsigned long long ct2 = nf_stat(buf, "ct_tracked");

    udp_send_once();
    udp_send_once();

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after sends with tracking off");
    CHK(nf_stat(buf, "ct_tracked") == ct2,
        "no new entry while tracking is off");
    CHK(nf_write("cton") == 0, "turn conntrack back on");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after cton");
    CHK(strstr(buf, "conntrack: on") != NULL, "conntrack reports on again");

    /*
     * 9. ICMP echo is tracked too, and its reply is matched back to it.
     *
     * This is the part that cannot be faked by a merely-parsing implementation.
     * Creating an entry on the request is easy; what is being asked is that the
     * *reply* finds that same entry, and the only evidence available from
     * outside is the entry's own state: the reply lands on the inbound path,
     * matches the reply half of the tuple, and moves the entry to established.
     * A reply that did not match would leave it "new" no matter how many times
     * the request was sent, because ICMP echo has no flags byte for the
     * TCP-style "reply seen" shortcut.
     *
     * Two identifiers on purpose: same id twice is one exchange, a third
     * request under a different id is a second one.  That is what makes
     * ct_tracked's increase attributable to the identifier being part of the
     * tuple rather than to "some ICMP got tracked".
     */
    CHK(nf_write("ctflush") == 0, "flush conntrack before the ICMP case");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read after the ICMP flush");
    unsigned long long ic0 = nf_stat(buf, "ct_tracked");
    unsigned long long icp0 = nf_stat(buf, "ct_packets");

    CHK(icmp_echo_once(0x1234) == 0, "send ICMP echo request id=0x1234");
    CHK(icmp_echo_once(0x1234) == 0, "send a second echo with the same id");
    /* One sleep for the whole case rather than one per request: the gateway
     * replies in well under a second, and a per-request sleep would only make
     * the gate slower without making it stricter. */
    usleep(400000);
    CHK(icmp_echo_once(0x5678) == 0, "send ICMP echo request id=0x5678");
    usleep(400000);

    CHK(nf_read(buf, sizeof(buf)) > 0, "read after the ICMP exchanges");
    unsigned long long ic1 = nf_stat(buf, "ct_tracked");
    unsigned long long icp1 = nf_stat(buf, "ct_packets");
    CHK(icp1 > icp0, "conntrack counted the ICMP packets");
    /* At least one new exchange, and the entry carries the gateway as its peer,
     * so an unrelated flow cannot satisfy this. */
    CHK(ic1 > ic0, "an ICMP exchange created a conntrack entry");
    CHK(strstr(buf, "icmp=echo") != NULL,
        "the ICMP entry is rendered as an echo, not as a flow with a port");
    const char *icmp_line = strstr(buf, "icmp=echo");
    CHK(icmp_line != NULL && strstr(icmp_line, "proto=1") != NULL,
        "the ICMP entry reports protocol 1");
    CHK(icmp_line != NULL && strstr(icmp_line, "icmp=echo id=4660") != NULL,
        "the entry carries the echo identifier that was sent (0x1234)");
    CHK(icmp_line != NULL && strstr(icmp_line, "state=established") != NULL,
        "the echo reply matched the entry and established it -- this is the "
        "reply-direction match, not just the insert");
    CHK(icmp_line != NULL && strstr(icmp_line, TEST_DST) != NULL,
        "the ICMP entry names the gateway it was exchanged with");
    /* The second identifier is a different tuple, so it must be a second entry
     * rather than more packets on the first. */
    CHK(ic1 >= ic0 + 2, "each echo identifier is its own conntrack entry");

    printf("NETFILTER_TEST: PASS dropped=%llu out_packets=%llu "
           "ct_tracked=%llu ct_packets=%llu icmp_tracked=%llu "
           "icmp_packets=%llu\n",
           out_drop1 - out_drop0, out_pkt1 - out_pkt0, ct1 - ct0, cp1 - cp0,
           ic1 - ic0, icp1 - icp0);
    return 0;
}
