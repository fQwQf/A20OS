/*
 * netctl — regression coverage for the user-space network control surface:
 * SIOCGIFCONF interface enumeration, the /proc/net/{dev,route} projections
 * that depend on it, and MSG_OOB rejection.
 *
 * The point of using musl's own <net/if.h> structs rather than private copies
 * is that the kernel then has to agree with the layout user space actually
 * compiles against -- that agreement is the compatibility contract, and a
 * self-consistent private pair would test nothing.
 *
 * Prints "NETCTL: PASS" on success.  The first deviation prints
 * "NETCTL: FAIL <what>" and returns 1.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifndef SIOCGIFCONF
#define SIOCGIFCONF 0x8912
#endif
#ifndef MSG_OOB
#define MSG_OOB 0x0001
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

static int fails;

static void fail(const char *what, long got)
{
    printf("NETCTL: FAIL %s (ret=%ld errno=%d)\n", what, got, errno);
    fails++;
}

/* Read one dotted-quad key from /proc/net/config.  Returns 0 on success. */
static int config_addr(const char *key, struct in_addr *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s=", key);
    FILE *f = fopen("/proc/net/config", "r");
    if (!f)
        return -1;
    char line[256];
    int rc = -1;
    size_t klen = strlen(path);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, path, klen) != 0)
            continue;
        if (inet_pton(AF_INET, line + klen, out) == 1)
            rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

/* SIOCGIFCONF must report a size, then fill exactly that many whole entries,
 * and every name it hands back must be resolvable by the per-interface
 * getters -- otherwise enumeration is useless to the tools that use it. */
static int test_gifconf(int fd, int *count_out)
{
    struct ifconf q;
    memset(&q, 0, sizeof(q));
    if (ioctl(fd, SIOCGIFCONF, &q) < 0) {
        fail("SIOCGIFCONF size query", -1);
        return -1;
    }
    if (q.ifc_len <= 0 || (q.ifc_len % (int)sizeof(struct ifreq)) != 0) {
        fail("SIOCGIFCONF size is a positive multiple of sizeof(ifreq)", q.ifc_len);
        return -1;
    }

    int want = q.ifc_len;
    char *buf = calloc(1, (size_t)want);
    if (!buf) {
        fail("calloc for ifconf buffer", -1);
        return -1;
    }
    struct ifconf c;
    memset(&c, 0, sizeof(c));
    c.ifc_len = want;
    c.ifc_buf = buf;
    if (ioctl(fd, SIOCGIFCONF, &c) < 0) {
        fail("SIOCGIFCONF fill", -1);
        free(buf);
        return -1;
    }
    if (c.ifc_len != want) {
        fail("SIOCGIFCONF reported fewer bytes than the size query promised", c.ifc_len);
        free(buf);
        return -1;
    }

    int n = want / (int)sizeof(struct ifreq);
    for (int i = 0; i < n; i++) {
        struct ifreq *ifr = &((struct ifreq *)buf)[i];
        if (ifr->ifr_name[0] == '\0') {
            fail("SIOCGIFCONF returned an empty interface name", i);
            free(buf);
            return -1;
        }
        struct ifreq probe;
        memset(&probe, 0, sizeof(probe));
        snprintf(probe.ifr_name, sizeof(probe.ifr_name), "%s", ifr->ifr_name);
        if (ioctl(fd, SIOCGIFADDR, &probe) < 0) {
            printf("NETCTL: FAIL enumerated name '%s' is not resolvable by "
                   "SIOCGIFADDR (errno=%d)\n", ifr->ifr_name, errno);
            fails++;
            free(buf);
            return -1;
        }
    }
    free(buf);
    *count_out = n;
    return 0;
}

/* /proc/net/dev must project the same interfaces SIOCGIFCONF enumerates. */
static void test_proc_net_dev(int expected)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) {
        fail("open /proc/net/dev", -1);
        return;
    }
    char line[512];
    int rows = 0;
    while (fgets(line, sizeof(line), f))
        if (strchr(line, ':'))
            rows++;
    fclose(f);
    if (rows < expected) {
        printf("NETCTL: FAIL /proc/net/dev has %d rows but SIOCGIFCONF "
               "enumerated %d\n", rows, expected);
        fails++;
    }
}

/* A configured gateway must actually appear in /proc/net/route; an empty
 * table under a configured gateway is the exact failure that made in-guest
 * network state unobservable. */
static void test_proc_net_route(void)
{
    struct in_addr gw;
    int have_gw = (config_addr("gateway", &gw) == 0) &&
                  (ntohl(gw.s_addr) != 0);
    if (!have_gw)
        return;

    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        fail("open /proc/net/route", -1);
        return;
    }
    char line[512];
    int rows = 0;
    int header = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!header) { header = 1; continue; }
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        /* Destination is field 2, gateway field 3; both non-zero means a
         * real default route row. */
        char *save = NULL;
        char *tok = strtok_r(line, " \t\n", &save);
        int field = 0, nonzero_gw = 0;
        while (tok) {
            field++;
            if (field == 3 && strtoul(tok, NULL, 16) != 0)
                nonzero_gw = 1;
            tok = strtok_r(NULL, " \t\n", &save);
        }
        if (nonzero_gw)
            rows++;
    }
    fclose(f);
    if (rows == 0) {
        printf("NETCTL: FAIL /proc/net/config reports a gateway but "
               "/proc/net/route has no route row\n");
        fails++;
    }
}

/* There is no out-of-band data path, so MSG_OOB must be refused rather than
 * silently satisfied with ordinary in-band bytes. */
static void test_msg_oob(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fail("socket for MSG_OOB test", -1);
        return;
    }
    char c = 0;
    errno = 0;
    long r = recv(fd, &c, 1, MSG_OOB | MSG_DONTWAIT);
    if (r >= 0 || errno != EOPNOTSUPP) {
        fail("recv(MSG_OOB) should be EOPNOTSUPP", r);
        close(fd);
        return;
    }
    errno = 0;
    r = send(fd, &c, 1, MSG_OOB | MSG_DONTWAIT);
    if (r >= 0 || errno != EOPNOTSUPP) {
        fail("send(MSG_OOB) should be EOPNOTSUPP", r);
        close(fd);
        return;
    }
    close(fd);
}

/* MSG_NOSIGNAL is accepted.  This stack never raises SIGPIPE on a socket, so
 * a dead-peer write reports EPIPE whether or not the flag is passed; the flag
 * must at least not be rejected as unknown. */
static void test_msg_nosignal(int fd)
{
    char c = 'x';
    errno = 0;
    long r = send(fd, &c, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r >= 0)
        return;                      /* datagram left; fine */
    if (errno == EOPNOTSUPP) {
        fail("send(MSG_NOSIGNAL) rejected as unsupported", r);
    }
}

/* ------------------------------------------------------------------ */
/* NETLINK_ROUTE -- the interface/address dump `ip` is built on.        */
/*                                                                     */
/* musl here ships no <linux/netlink.h>, so the wire structs are        */
/* restated below: an independent statement of the contract is what     */
/* makes this a test of the kernel rather than a mirror of it.          */
/* ------------------------------------------------------------------ */

#define NETLINK_ROUTE_   0
#define NLT_DONE         3
#define NLT_GETLINK      18
#define NLT_GETADDR      22
#define NLT_IFNAME       3

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

struct rtattr {
    uint16_t rta_len;
    uint16_t rta_type;
};

struct sockaddr_nl {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

struct ifinfomsg {
    uint8_t  ifi_family;
    uint8_t  ifi_pad;
    uint16_t ifi_type;
    int32_t  ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

struct ifaddrmsg {
    uint8_t  ifa_family;
    uint8_t  ifa_prefixlen;
    uint8_t  ifa_flags;
    uint8_t  ifa_scope;
    uint32_t ifa_index;
};

/* *str, when asked for, receives a pointer to the attribute payload -- i.e.
 * past the 4-byte rtattr header. */
static const struct rtattr *find_attr(const void *payload, size_t len,
                                      uint16_t want, const char **str)
{
    const uint8_t *p = payload;
    size_t off = 0;
    while (off + 4 <= len) {
        uint16_t alen, atype;
        memcpy(&alen, p + off, sizeof(alen));
        memcpy(&atype, p + off + 2, sizeof(atype));
        if (alen < 4 || off + alen > len)
            return NULL;
        if (atype == want) {
            if (str)
                *str = (const char *)(p + off + 4);
            return (const struct rtattr *)(p + off);
        }
        off += (alen + 3) & ~(size_t)3;
    }
    return NULL;
}

static int rtnetlink_dump(int type, const char *label, int want_names)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE_);
    if (fd < 0) {
        printf("NETCTL: FAIL socket(AF_NETLINK, ROUTE) errno=%d\n", errno);
        fails++;
        return -1;
    }
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("NETCTL: FAIL bind(AF_NETLINK) errno=%d\n", errno);
        fails++;
        close(fd);
        return -1;
    }

    uint8_t req[sizeof(struct nlmsghdr) + sizeof(struct ifinfomsg)];
    memset(req, 0, sizeof(req));
    struct nlmsghdr *nl = (struct nlmsghdr *)req;
    nl->nlmsg_len = (uint32_t)(sizeof(struct nlmsghdr) + sizeof(struct ifinfomsg));
    nl->nlmsg_type = (uint16_t)type;
    nl->nlmsg_flags = 0;
    nl->nlmsg_seq = 1;
    nl->nlmsg_pid = 0;
    if (send(fd, req, nl->nlmsg_len, 0) < 0) {
        printf("NETCTL: FAIL send(%s) errno=%d\n", label, errno);
        fails++;
        close(fd);
        return -1;
    }

    int links = 0, named = 0, done = 0;
    for (;;) {
        uint8_t buf[2048];
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        if (r < 0) {
            printf("NETCTL: FAIL recv(%s) errno=%d\n", label, errno);
            fails++;
            break;
        }
        for (ssize_t off = 0; off + (ssize_t)sizeof(struct nlmsghdr) <= r; ) {
            struct nlmsghdr h;
            memcpy(&h, buf + off, sizeof(h));
            if (h.nlmsg_len < sizeof(h) || off + (ssize_t)h.nlmsg_len > r)
                break;
            if (h.nlmsg_type == NLT_DONE) {
                done = 1;
                break;
            }
            if (h.nlmsg_type == (uint16_t)type) {
                links++;
                const void *payload = buf + off + sizeof(h);
                size_t plen = h.nlmsg_len - sizeof(h);
                if (type == NLT_GETLINK) {
                    if (plen < sizeof(struct ifinfomsg))
                        continue;
                    const char *name = NULL;
                    if (find_attr(payload + sizeof(struct ifinfomsg),
                                  plen - sizeof(struct ifinfomsg),
                                  NLT_IFNAME, &name) && name && *name)
                        named++;
                } else if (plen < sizeof(struct ifaddrmsg)) {
                    continue;
                }
            }
            off += (ssize_t)((h.nlmsg_len + 3) & ~(uint32_t)3);
        }
        if (done)
            break;
    }
    close(fd);

    if (!done) {
        printf("NETCTL: FAIL %s dump ended without NLMSG_DONE\n", label);
        fails++;
        return -1;
    }
    if (links == 0) {
        printf("NETCTL: FAIL %s dump returned no objects\n", label);
        fails++;
        return -1;
    }
    if (want_names && named != links) {
        printf("NETCTL: FAIL %s dump: %d links but only %d carried "
               "IFLA_IFNAME\n", label, links, named);
        fails++;
        return -1;
    }
    return links;
}

static void test_rtnetlink(void)
{
    int links = rtnetlink_dump(NLT_GETLINK, "RTM_GETLINK", 1);
    int addrs = rtnetlink_dump(NLT_GETADDR, "RTM_GETADDR", 0);
    /* The configured interface must have an address object, otherwise `ip addr`
     * would show the interface with no IPv4 even though SIOCGIFCONF reports one. */
    if (links > 0 && addrs == 0) {
        printf("NETCTL: FAIL %d links but no RTM_GETADDR objects\n", links);
        fails++;
    }
}

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("NETCTL: FAIL socket(AF_INET) failed errno=%d\n", errno);
        return 1;
    }

    int count = 0;
    if (test_gifconf(fd, &count) == 0) {
        test_proc_net_dev(count);
        test_msg_nosignal(fd);
    }
    test_proc_net_route();
    test_msg_oob();
    test_rtnetlink();
    close(fd);

    if (fails) {
        printf("NETCTL: FAIL (%d failing checks)\n", fails);
        return 1;
    }
    printf("NETCTL: PASS (%d interfaces enumerated)\n", count);
    return 0;
}
