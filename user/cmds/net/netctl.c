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
#include <poll.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
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
#define NLT_ERROR        2
#define NLT_DONE         3
#define NLT_NEWLINK      16
#define NLT_GETLINK      18
#define NLT_NEWADDR      20
#define NLT_GETADDR      22
#define NLT_IFA_LOCAL    2
#define NLT_REQUEST_ACK  0x005
#define NLT_CREATE_EXCL  0x600
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

/* Configure a second link in the RAM-only router test image. */
static int set_ipv4(int fd, const char *name, const char *ip, const char *mask)
{
    struct ifreq request = {0};
    struct sockaddr_in address = {.sin_family = AF_INET};
    struct in_addr parsed_ip;
    if (strlen(name) >= sizeof(request.ifr_name) ||
        inet_pton(AF_INET, ip, &parsed_ip) != 1 ||
        inet_pton(AF_INET, mask, &address.sin_addr) != 1) {
        fprintf(stderr, "NETCTL: FAIL invalid IPv4 configuration\n");
        return 1;
    }
    strcpy(request.ifr_name, name);
    memcpy(&request.ifr_netmask, &address, sizeof(address));
    if (ioctl(fd, SIOCSIFNETMASK, &request) == 0) {
        address.sin_addr = parsed_ip;
        memcpy(&request.ifr_addr, &address, sizeof(address));
        if (ioctl(fd, SIOCSIFADDR, &request) == 0) {
            puts("NETCTL: IPv4 configured");
            return 0;
        }
    }
    perror("NETCTL: FAIL configure IPv4");
    return 1;
}

static int rtnetlink_ack(const void *request, size_t length)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE_);
    if (fd < 0)
        return -1;
    struct sockaddr_nl local = {.nl_family = AF_NETLINK};
    struct {
        struct nlmsghdr header;
        int32_t error;
        struct nlmsghdr request;
    } ack;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0 &&
        send(fd, request, length, 0) == (ssize_t)length) {
        ssize_t count = recv(fd, &ack, sizeof(ack), 0);
        if (count >= (ssize_t)(sizeof(ack.header) + sizeof(ack.error)) &&
            ack.header.nlmsg_seq == 1 && ack.header.nlmsg_type == NLT_ERROR) {
            if (ack.error == 0) {
                close(fd);
                return 0;
            }
            errno = -ack.error;
        } else if (count >= 0) {
            errno = EPROTO;
        }
    }
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

static int set_ipv6(const char *name, const char *ip, const char *prefix)
{
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg address;
        struct rtattr local;
        struct in6_addr value;
    } request = {0};
    char *end;
    unsigned long bits = strtoul(prefix, &end, 10);
    request.address.ifa_index = if_nametoindex(name);
    if (!request.address.ifa_index || !*prefix || *end || bits > 128 ||
        inet_pton(AF_INET6, ip, &request.value) != 1) {
        fprintf(stderr, "NETCTL: FAIL invalid IPv6 configuration\n");
        return 1;
    }
    request.header = (struct nlmsghdr){sizeof(request), NLT_NEWADDR,
                                      NLT_REQUEST_ACK | NLT_CREATE_EXCL, 1, 0};
    request.address.ifa_family = AF_INET6;
    request.address.ifa_prefixlen = (uint8_t)bits;
    request.local = (struct rtattr){sizeof(request.local) + sizeof(request.value), NLT_IFA_LOCAL};

    if (rtnetlink_ack(&request, sizeof(request)) < 0) {
        perror("NETCTL: FAIL configure IPv6");
        return 1;
    }
    puts("NETCTL: IPv6 configured");
    return 0;
}

static int set_link(const char *name, const char *state)
{
    unsigned index = if_nametoindex(name);
    int up = !strcmp(state, "up");
    if (!index || (!up && strcmp(state, "down"))) {
        fprintf(stderr, "NETCTL: FAIL invalid link configuration\n");
        return 1;
    }
    struct {
        struct nlmsghdr header;
        struct ifinfomsg link;
    } request = {
        .header = {sizeof(request), NLT_NEWLINK, NLT_REQUEST_ACK, 1, 0},
        .link = {.ifi_index = (int32_t)index, .ifi_change = IFF_UP,
                 .ifi_flags = up ? IFF_UP : 0},
    };
    if (rtnetlink_ack(&request, sizeof(request)) < 0) {
        perror("NETCTL: FAIL configure link");
        return 1;
    }
    puts("NETCTL: link configured");
    return 0;
}

static int netlink_watch(const char *name)
{
    enum { LEVEL_NETLINK = 270, ADD_MEMBERSHIP = 1, DROP_MEMBERSHIP = 2, LINK_GROUP = 1 };
    unsigned index = if_nametoindex(name);
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE_);
    int group = LINK_GROUP;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct sockaddr_nl address;
    socklen_t size = sizeof(address);
    if (!index || fd < 0 ||
        connect(fd, (struct sockaddr *)&kernel, sizeof(kernel)) < 0 ||
        getpeername(fd, (struct sockaddr *)&address, &size) < 0 ||
        size != sizeof(address) || address.nl_family != AF_NETLINK ||
        address.nl_pid != 0 || address.nl_groups != 0 ||
        setsockopt(fd, LEVEL_NETLINK, ADD_MEMBERSHIP, &group, sizeof(group)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &size) < 0 ||
        address.nl_pid == 0 || address.nl_groups != 1) {
        perror("NETCTL: FAIL subscribe link");
        if (fd >= 0) close(fd);
        return 1;
    }
    struct {
        struct nlmsghdr header;
        struct ifinfomsg link;
    } request = {
        .header = {sizeof(request), NLT_GETLINK, 1, 73, 0},
        .link = {.ifi_index = (int32_t)index},
    };
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    uint8_t buffer[512];
    for (int query = 1; query >= 0; --query) {
        if (query && send(fd, &request, sizeof(request), 0) != sizeof(request))
            goto fail;
        if (poll(&ready, 1, 5000) != 1)
            goto fail;
        ssize_t length = recv(fd, buffer, sizeof(buffer), 0);
        struct nlmsghdr header;
        struct ifinfomsg link;
        if (length < (ssize_t)(sizeof(header) + sizeof(link)))
            goto fail;
        memcpy(&header, buffer, sizeof(header));
        memcpy(&link, buffer + sizeof(header), sizeof(link));
        if (header.nlmsg_type != NLT_NEWLINK || link.ifi_index != (int32_t)index)
            goto fail;
        if (query) {
            if (header.nlmsg_seq != 73)
                goto fail;
            puts("NETCTL: link watch ready");
            fflush(stdout);
        } else if (link.ifi_flags & IFF_UP) {
            goto fail;
        }
    }
    if (setsockopt(fd, LEVEL_NETLINK, DROP_MEMBERSHIP, &group, sizeof(group)) < 0 ||
        getsockname(fd, (struct sockaddr *)&address, &size) < 0 || address.nl_groups)
        goto fail;
    puts("NETCTL: link watch dropped");
    fflush(stdout);
    if (poll(&ready, 1, 1800) != 0)
        goto fail;
    close(fd);
    puts("NETCTL: link watch passed");
    return 0;
fail:
    perror("NETCTL: FAIL link watch");
    close(fd);
    return 1;
}

static int raw6_echo(const char *name)
{
    unsigned index = if_nametoindex(name);
    int fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (!index || fd < 0)
        goto fail;
    struct icmp6_filter filter;
    ICMP6_FILTER_SETBLOCKALL(&filter);
    ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
    struct ipv6_mreq membership = {.ipv6mr_interface = index};
    inet_pton(AF_INET6, "ff02::1", &membership.ipv6mr_multiaddr);
    int checksum = 2, hops = 255, one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name)) < 0 ||
        setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership, sizeof(membership)) < 0 ||
        setsockopt(fd, IPPROTO_RAW, IPV6_CHECKSUM, &checksum, sizeof(checksum)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) < 0)
        goto fail;

    struct sockaddr_in6 target = {.sin6_family = AF_INET6, .sin6_scope_id = index};
    inet_pton(AF_INET6, "ff02::2", &target.sin6_addr);
    const uint8_t request[] = {128, 0, 0, 0, 0xa2, 0x06, 0, 1, 'r', 'a', 'w'};
    if (sendto(fd, request, sizeof(request), 0, (void *)&target, sizeof(target)) != sizeof(request))
        goto fail;

    struct pollfd ready = {.fd = fd, .events = POLLIN};
    if (poll(&ready, 1, 5000) != 1)
        goto fail;
    uint8_t packet[64];
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct sockaddr_in6 peer;
    struct iovec iov = {.iov_base = packet, .iov_len = sizeof(packet)};
    struct msghdr reply = {
        .msg_name = &peer, .msg_namelen = sizeof(peer),
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
    };
    ssize_t received = recvmsg(fd, &reply, 0);
    if (received != sizeof(request) || packet[0] != ICMP6_ECHO_REPLY ||
        memcmp(packet + 4, request + 4, sizeof(request) - 4) || peer.sin6_scope_id != index) {
        fprintf(stderr, "raw reply size=%ld type=%u scope=%u expected=%u\n",
                (long)received, received > 0 ? packet[0] : 0,
                received >= 0 ? peer.sin6_scope_id : 0, index);
        goto fail;
    }
    struct cmsghdr *meta = CMSG_FIRSTHDR(&reply);
    if (!meta || meta->cmsg_level != IPPROTO_IPV6 || meta->cmsg_type != IPV6_HOPLIMIT ||
        meta->cmsg_len != CMSG_LEN(sizeof(int))) {
        fprintf(stderr, "raw reply ancillary length=%zu\n", (size_t)reply.msg_controllen);
        goto fail;
    }
    memcpy(&hops, CMSG_DATA(meta), sizeof(hops));
    if (hops != 64) {
        fprintf(stderr, "raw reply hop limit=%d\n", hops);
        goto fail;
    }
    close(fd);
    puts("NETCTL: raw IPv6 echo passed");
    return 0;
fail:
    perror("NETCTL: FAIL raw IPv6 echo");
    if (fd >= 0) close(fd);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "raw6-echo"))
        return raw6_echo(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "watch-link"))
        return netlink_watch(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "set-link"))
        return set_link(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "set-ipv6"))
        return set_ipv6(argv[2], argv[3], argv[4]);
    if (argc != 1 && (argc != 5 || strcmp(argv[1], "set-ipv4"))) {
        fprintf(stderr, "usage: netctl [set-ipv4 IF ADDRESS NETMASK | set-ipv6 IF ADDRESS PREFIX | set-link IF up|down | watch-link IF | raw6-echo IF]\n");
        return 2;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("NETCTL: FAIL socket(AF_INET) failed errno=%d\n", errno);
        return 1;
    }

    if (argc == 5) {
        int result = set_ipv4(fd, argv[2], argv[3], argv[4]);
        close(fd);
        return result;
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
