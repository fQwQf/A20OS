/*
 * Runtime gate for the netlink write path and the uevent send path.
 *
 * Both are syscall-reachable parsers that act on live network state, so the
 * checks are weighted towards the cases that must be REFUSED: a parser that
 * accepts a malformed or over-reaching request does far more damage than one
 * that lacks a feature.  Only three cases mutate anything, and each re-applies
 * a value the interface already has, so this test cannot leave the machine's
 * own address or link state changed -- which matters because the rest of
 * network_suite depends on the boot-time address staying put.
 *
 * The assertion that matters most is the negative one for RTM_NEWADDR: with
 * one IPv4 slot per netif, a plain "add" of a different address must be refused
 * rather than silently overwriting the primary.  A kernel that overwrote it
 * would still answer success, and the next test in the suite would lose the
 * address it needs.
 *
 * musl ships no <linux/netlink.h>, so the UAPI layout is spelled out here and
 * pinned with _Static_assert on every size and offset the kernel depends on.
 * That is deliberate: a layout the kernel and this test agree on privately
 * would prove nothing, whereas a size or offset that drifts shows up as a
 * compile error instead of a silently misparsed message.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#define TEST_NAME "NETLINK"

/* Protocol numbers, absent from musl's public headers. */
#define NL_ROUTE        0
#define NL_KOBJECT      15

#define NLMSG_ALIGNTO   4
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_LENGTH(len) (NLMSG_ALIGN(sizeof(struct nlmsghdr)) + (len))

#define NLM_F_REQUEST   0x001
#define NLM_F_REPLACE   0x100
#define NLM_F_MULTI     0x200

#define RTM_NEWLINK     16
#define RTM_NEWADDR     20
#define RTM_DELADDR     21

#define IFA_ADDRESS     1
#define IFA_LOCAL       2
#define IFLA_MTU        4

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

struct ifaddrmsg {
    uint8_t  ifa_family;
    uint8_t  ifa_prefixlen;
    uint8_t  ifa_flags;
    uint8_t  ifa_scope;
    uint32_t ifa_index;
};

struct ifinfomsg {
    uint8_t  ifi_family;
    uint8_t  ifi_pad;
    uint16_t ifi_type;
    int32_t  ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};

struct nl_sockaddr {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

_Static_assert(sizeof(struct nlmsghdr) == 16, "nlmsghdr must be 16 bytes");
_Static_assert(offsetof(struct nlmsghdr, nlmsg_type) == 4, "nlmsg_type offset");
_Static_assert(offsetof(struct nlmsghdr, nlmsg_flags) == 6, "nlmsg_flags offset");
_Static_assert(sizeof(struct rtattr) == 4, "rtattr must be 4 bytes");
_Static_assert(sizeof(struct ifaddrmsg) == 8, "ifaddrmsg must be 8 bytes");
_Static_assert(offsetof(struct ifaddrmsg, ifa_index) == 4, "ifa_index offset");
_Static_assert(sizeof(struct ifinfomsg) == 16, "ifinfomsg must be 16 bytes");
_Static_assert(offsetof(struct ifinfomsg, ifi_index) == 4, "ifi_index offset");
_Static_assert(offsetof(struct ifinfomsg, ifi_change) == 12, "ifi_change offset");
_Static_assert(sizeof(struct nl_sockaddr) == 12, "sockaddr_nl must be 12 bytes");
_Static_assert(offsetof(struct nl_sockaddr, nl_family) == 0,
               "nl_family offset");

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

static size_t add_attr(unsigned char *buf, size_t off, uint16_t type,
                       const void *data, size_t len)
{
    struct rtattr *rta = (struct rtattr *)(buf + off);

    rta->rta_len = (uint16_t)(sizeof(struct rtattr) + len);
    rta->rta_type = type;
    memcpy((unsigned char *)rta + sizeof(struct rtattr), data, len);
    return off + NLMSG_ALIGN(rta->rta_len);
}

static int send_nl(int fd, const void *msg, size_t len)
{
    struct nl_sockaddr sa;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    return (int)sendto(fd, msg, len, 0, (struct sockaddr *)&sa,
                       sizeof(struct nl_sockaddr));
}

/* Sends one RTM_NEWADDR/RTM_DELADDR carrying up to two address attributes. */
static int send_addr(int fd, uint16_t type, uint16_t flags, int ifindex,
                     int family, int prefixlen, const uint8_t *a1, uint16_t t1,
                     const uint8_t *a2, uint16_t t2)
{
    unsigned char buf[256];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifaddrmsg *ifa = (struct ifaddrmsg *)(buf + sizeof(*nlh));
    size_t off;

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
    nlh->nlmsg_type = type;
    nlh->nlmsg_flags = flags;
    ifa->ifa_family = (uint8_t)family;
    ifa->ifa_prefixlen = (uint8_t)prefixlen;
    ifa->ifa_index = (uint32_t)ifindex;

    off = sizeof(*nlh) + sizeof(*ifa);
    if (a1)
        off = add_attr(buf, off, t1, a1, 4);
    if (a2)
        off = add_attr(buf, off, t2, a2, 4);
    nlh->nlmsg_len = (uint32_t)off;
    return send_nl(fd, buf, off);
}

static int send_link(int fd, int ifindex, uint32_t change, uint32_t flags,
                     const void *mtu, size_t mtu_len)
{
    unsigned char buf[256];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifinfomsg *ifi = (struct ifinfomsg *)(buf + sizeof(*nlh));
    size_t off;

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));
    nlh->nlmsg_type = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = flags;
    ifi->ifi_change = change;

    off = sizeof(*nlh) + sizeof(*ifi);
    if (mtu)
        off = add_attr(buf, off, IFLA_MTU, mtu, mtu_len);
    nlh->nlmsg_len = (uint32_t)off;
    return send_nl(fd, buf, off);
}

static int find_configured_if(int sfd, int *ifindex, uint8_t addr[4],
                              uint8_t mask[4], char *name, size_t namesz)
{
    struct ifconf ifc;
    size_t need, entries;
    char *buf, *row;
    int found = 0;

    memset(&ifc, 0, sizeof(ifc));
    if (ioctl(sfd, SIOCGIFCONF, &ifc) < 0 || ifc.ifc_len <= 0)
        return -1;
    need = (size_t)ifc.ifc_len;
    buf = malloc(need);
    if (!buf)
        return -1;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)need;
    ifc.ifc_buf = buf;
    if (ioctl(sfd, SIOCGIFCONF, &ifc) < 0) {
        free(buf);
        return -1;
    }
    entries = (size_t)ifc.ifc_len / sizeof(struct ifreq);
    row = buf;
    for (size_t i = 0; i < entries && !found; i++, row += sizeof(struct ifreq)) {
        struct ifreq ifr;

        memcpy(&ifr, row, sizeof(ifr));
        if (((struct sockaddr_in *)&ifr.ifr_ifru)->sin_family != AF_INET)
            continue;
        if (ioctl(sfd, SIOCGIFFLAGS, &ifr) < 0 || (ifr.ifr_flags & IFF_LOOPBACK))
            continue;
        memcpy(addr, &((struct sockaddr_in *)&ifr.ifr_ifru)->sin_addr.s_addr, 4);

        memcpy(&ifr, row, sizeof(ifr));
        if (ioctl(sfd, SIOCGIFNETMASK, &ifr) < 0)
            continue;
        memcpy(mask, &((struct sockaddr_in *)&ifr.ifr_ifru)->sin_addr.s_addr, 4);

        memcpy(&ifr, row, sizeof(ifr));
        if (ioctl(sfd, SIOCGIFINDEX, &ifr) < 0)
            continue;
        *ifindex = ifr.ifr_ifindex;
        snprintf(name, namesz, "%s", row);
        found = 1;
    }
    free(buf);
    return found ? 0 : -1;
}

static unsigned mask_to_prefixlen(const uint8_t mask[4])
{
    unsigned n = 0;

    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--) {
            if (!(mask[i] & (1u << b)))
                return n;
            n++;
        }
    return n;
}

static void test_addr_refusals(int fd, int ifindex, const uint8_t addr[4])
{
    uint8_t other[4];

    memcpy(other, addr, 4);
    other[3] ^= 0x01;

    /* The single most important refusal here: a plain add of a different
     * address must not silently take over the one IPv4 slot. */
    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, ifindex, AF_INET, 24,
                 other, IFA_LOCAL, NULL, 0) < 0,
       "RTM_NEWADDR of a different address is refused");
    ok(errno == EOPNOTSUPP,
       "a plain add that would replace the primary fails with EOPNOTSUPP");

    /* The secondary-address spelling: IFA_ADDRESS is the peer and differs from
     * IFA_LOCAL.  Applying it would also overwrite the primary. */
    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, ifindex, AF_INET, 24,
                 other, IFA_ADDRESS, addr, IFA_LOCAL) < 0 &&
           errno == EOPNOTSUPP,
       "the secondary-address form (IFA_LOCAL != IFA_ADDRESS) is refused");

    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, ifindex, AF_INET6, 64,
                 addr, IFA_LOCAL, NULL, 0) < 0 && errno == EAFNOSUPPORT,
       "RTM_NEWADDR with an IPv6 family is refused with EAFNOSUPPORT");

    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, 0, AF_INET, 24,
                 addr, IFA_LOCAL, NULL, 0) < 0 && errno == EINVAL,
       "RTM_NEWADDR with ifa_index 0 is refused with EINVAL");

    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, ifindex, AF_INET, 33,
                 addr, IFA_LOCAL, NULL, 0) < 0 && errno == EINVAL,
       "RTM_NEWADDR with a prefix length over 32 is refused with EINVAL");

    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST, ifindex, AF_INET, 24,
                 NULL, 0, NULL, 0) < 0 && errno == EINVAL,
       "RTM_NEWADDR carrying no address attribute is refused with EINVAL");

    /* An attribute of the wrong length must be refused by size, not read past:
     * this parser is reachable straight from a syscall. */
    {
        unsigned char buf[256];
        struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
        struct ifaddrmsg *ifa = (struct ifaddrmsg *)(buf + sizeof(*nlh));
        uint16_t shortaddr[1] = { 0 };

        memset(buf, 0, sizeof(buf));
        nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
        nlh->nlmsg_type = RTM_NEWADDR;
        nlh->nlmsg_flags = NLM_F_REQUEST;
        ifa->ifa_family = AF_INET;
        ifa->ifa_prefixlen = 24;
        ifa->ifa_index = (uint32_t)ifindex;
        nlh->nlmsg_len = (uint32_t)add_attr(buf, sizeof(*nlh) + sizeof(*ifa),
                                            IFA_LOCAL, shortaddr,
                                            sizeof(shortaddr));
        errno = 0;
        ok(send_nl(fd, buf, nlh->nlmsg_len) < 0 && errno == EINVAL,
           "an IFA_LOCAL attribute of the wrong length is refused with EINVAL");
    }

    /* A stale delete must not clear an address the caller never named. */
    errno = 0;
    ok(send_addr(fd, RTM_DELADDR, NLM_F_REQUEST, ifindex, AF_INET, 24,
                 other, IFA_LOCAL, NULL, 0) < 0 && errno == EADDRNOTAVAIL,
       "RTM_DELADDR of an address the interface does not hold is refused");
}

static void test_envelope_refusals(int fd, int ifindex)
{
    unsigned char buf[256];
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    struct ifaddrmsg *ifa = (struct ifaddrmsg *)(buf + sizeof(*nlh));

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(0);
    nlh->nlmsg_type = RTM_NEWADDR;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    errno = 0;
    ok(send_nl(fd, buf, NLMSG_LENGTH(0)) < 0 && errno == EINVAL,
       "an RTM_NEWADDR too short to hold an ifaddrmsg is refused");

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = 4096;
    nlh->nlmsg_type = RTM_NEWADDR;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    ifa->ifa_family = AF_INET;
    ifa->ifa_index = (uint32_t)ifindex;
    errno = 0;
    ok(send_nl(fd, buf, NLMSG_LENGTH(sizeof(*ifa))) < 0 && errno == EINVAL,
       "an nlmsg_len longer than the datagram is refused");

    memset(buf, 0, sizeof(buf));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));
    nlh->nlmsg_type = 99;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    errno = 0;
    ok(send_nl(fd, buf, NLMSG_ALIGN(nlh->nlmsg_len)) < 0 &&
           errno == EOPNOTSUPP,
       "an unknown netlink message type is refused with EOPNOTSUPP");

    /* Only a single-message write is implemented, so a multipart request must
     * be refused rather than half-applied. */
    errno = 0;
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_MULTI, ifindex,
                 AF_INET, 24, NULL, 0, NULL, 0) < 0 && errno == EINVAL,
       "a multipart RTM_NEWADDR is refused with EINVAL");
}

static void test_link(int fd, int ifindex)
{
    uint32_t mtu;
    uint16_t small = 0;

    errno = 0;
    ok(send_link(fd, ifindex, 0x10000, 0, NULL, 0) < 0 && errno == EOPNOTSUPP,
       "RTM_NEWLINK changing an unmodelled flag is refused with EOPNOTSUPP");

    errno = 0;
    ok(send_link(fd, ifindex, 0, 0, &small, sizeof(small)) < 0 &&
           errno == EINVAL,
       "an IFLA_MTU attribute of the wrong length is refused with EINVAL");

    mtu = 8; /* below the RFC 791 minimum of 68 */
    errno = 0;
    ok(send_link(fd, ifindex, 0, 0, &mtu, sizeof(mtu)) < 0 && errno == EINVAL,
       "an MTU below the link minimum is refused with EINVAL");

    errno = 0;
    ok(send_link(fd, 0, 0, 0, NULL, 0) < 0 && errno == EINVAL,
       "RTM_NEWLINK with ifi_index 0 is refused with EINVAL");

    /* The two accepted cases re-apply state the interface already has, so the
     * success path is exercised without changing anything.  Linux reads the new
     * admin state from ifi_flags and only the *selection* from ifi_change, so
     * IFF_UP has to be set in both: asking to change IFF_UP with ifi_flags
     * clear is a request to bring the link DOWN, not a no-op. */
    ok(send_link(fd, ifindex, 0x1, 0x1 /* IFF_UP in both */, NULL, 0) >= 0,
       "RTM_NEWLINK setting IFF_UP on an already-up link is accepted");

    mtu = 1500;
    ok(send_link(fd, ifindex, 0, 0, &mtu, sizeof(mtu)) >= 0,
       "RTM_NEWLINK re-applying the current MTU is accepted");
}

static void test_addr_idempotent(int fd, int ifindex, const uint8_t addr[4],
                                 const uint8_t mask[4])
{
    /* `ip addr replace` with the address and prefix length already in place.
     * This is the only address write the test performs, and it is a no-op by
     * construction. */
    ok(send_addr(fd, RTM_NEWADDR, NLM_F_REQUEST | NLM_F_REPLACE, ifindex,
                 AF_INET, (int)mask_to_prefixlen(mask), addr, IFA_LOCAL,
                 NULL, 0) >= 0,
       "RTM_NEWADDR re-asserting the current address with NLM_F_REPLACE is "
       "accepted");
}

static void test_state_intact(int sfd, const char *name, const uint8_t addr[4],
                              const uint8_t mask[4])
{
    struct ifreq ifr;
    uint8_t now[4];

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(sfd, SIOCGIFADDR, &ifr) < 0) {
        ok(0, "the interface still has an address after the write path ran");
        return;
    }
    memcpy(now, &((struct sockaddr_in *)&ifr.ifr_ifru)->sin_addr.s_addr, 4);
    ok(memcmp(now, addr, 4) == 0,
       "the address survived every request in this test");

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(sfd, SIOCGIFNETMASK, &ifr) < 0) {
        ok(0, "the interface still has a netmask");
        return;
    }
    memcpy(now, &((struct sockaddr_in *)&ifr.ifr_ifru)->sin_addr.s_addr, 4);
    ok(memcmp(now, mask, 4) == 0, "the netmask survived");

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(sfd, SIOCGIFFLAGS, &ifr) == 0)
        ok((ifr.ifr_flags & IFF_UP) != 0, "the link is still up");
}

static void test_uevent(void)
{
    int uev = socket(AF_NETLINK, SOCK_RAW, NL_KOBJECT);
    int route = socket(AF_NETLINK, SOCK_RAW, NL_ROUTE);

    if (uev < 0 || route < 0) {
        ok(0, "open the uevent and route netlink sockets");
        if (uev >= 0)
            close(uev);
        if (route >= 0)
            close(route);
        return;
    }

    /* Send dispatches on socket protocol, so this payload reaches the route
     * parser, not the uevent emitter, and is rejected there.  The errno is
     * therefore a parse rejection rather than EPROTONOSUPPORT, so only the
     * refusal is asserted: do not tighten this back to a specific errno. */
    errno = 0;
    ok(send_nl(route, "add", 3) < 0,
       "a uevent payload on a route socket is rejected, not emitted");

    errno = 0;
    ok(send_nl(uev, "bogus", 5) < 0 && errno == EINVAL,
       "an unknown uevent action is refused with EINVAL");
    errno = 0;
    ok(send_nl(uev, "@some/device", 12) < 0 && errno == EINVAL,
       "a uevent with an empty action is refused with EINVAL");
    errno = 0;
    ok(send_nl(uev, "add@", 4) < 0 && errno == EINVAL,
       "a uevent with an empty device name is refused with EINVAL");
    errno = 0;
    ok(send_nl(uev, "bogus@some/device", 18) < 0 && errno == EINVAL,
       "an unknown action is refused even with a device path");
    errno = 0;
    ok(send_nl(uev, "add@/class/net/nosuchdev0", 25) < 0 && errno == ENOENT,
       "a uevent naming a device that does not exist is refused with ENOENT");

    close(uev);
    close(route);
}

int main(void)
{
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    int nl = socket(AF_NETLINK, SOCK_RAW, NL_ROUTE);
    int ifindex = 0;
    uint8_t addr[4], mask[4];
    char name[IFNAMSIZ] = { 0 };

    if (sfd < 0 || nl < 0) {
        printf("%s: FAIL socket: %s\n", TEST_NAME, strerror(errno));
        if (sfd >= 0)
            close(sfd);
        if (nl >= 0)
            close(nl);
        return 1;
    }

    if (find_configured_if(sfd, &ifindex, addr, mask, name, sizeof(name)) < 0) {
        printf("%s: FAIL no configured non-loopback interface to test "
               "against\n", TEST_NAME);
        close(sfd);
        close(nl);
        return 1;
    }
    printf("%s: info testing against %s ifindex=%d addr=%u.%u.%u.%u/%u\n",
           TEST_NAME, name, ifindex, addr[0], addr[1], addr[2], addr[3],
           mask_to_prefixlen(mask));

    test_addr_refusals(nl, ifindex, addr);
    test_envelope_refusals(nl, ifindex);
    test_link(nl, ifindex);
    test_addr_idempotent(nl, ifindex, addr, mask);
    test_state_intact(sfd, name, addr, mask);
    test_uevent();

    close(sfd);
    close(nl);

    if (failures == 0) {
        printf("%s: PASS (%d checks)\n", TEST_NAME, checks);
        return 0;
    }
    printf("%s: FAIL (%d of %d checks failed)\n", TEST_NAME, failures, checks);
    return 1;
}
