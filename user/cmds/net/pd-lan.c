/*
 * Publish DHCPv6 delegated prefixes to configured Ethernet LANs.
 * odhcp6c owns the WAN protocol. Its hook sends parsed lease snapshots over
 * a private inherited pipe. This process owns the LAN addresses and RAs.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/icmp6.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_LANS = 8, MAX_LEASES = 8, MAX_PREFIXES = 16,
    MAX_PIO_VALID = 7200, MAX_PIO_PREFERRED = 2700,
    RA_MIN_SPACING = 3000, RA_ROUTER_LIFETIME = 30,
};

struct lease {
    struct in6_addr prefix, excluded;
    uint64_t preferred_until, valid_until;
    unsigned length, excluded_length;
};

struct update {
    unsigned count;
    struct lease leases[MAX_LEASES];
};

_Static_assert(sizeof(struct update) <= 4096, "lease snapshots must fit a pipe write");

struct prefix {
    struct in6_addr network;
    uint64_t preferred_until, valid_until, withdraw_until;
    bool installed, wanted;
};

struct lan {
    char name[IF_NAMESIZE];
    unsigned index;
    int fd;
    uint64_t subnet;
    uint8_t mac[6];
    uint64_t next_ra, last_ra;
    struct prefix prefixes[MAX_PREFIXES];
};

static volatile sig_atomic_t stopping;
static unsigned random_seed;

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t remaining(uint64_t until, uint64_t now)
{
    return until > now ? (until - now) / 1000 : 0;
}

static uint32_t bounded(uint32_t value, uint32_t limit)
{
    return value < limit ? value : limit;
}

static bool number(const char *text, int base, uint64_t maximum, uint64_t *value)
{
    char *end;
    if (*text < '0' || *text > '9')
        return false;
    errno = 0;
    unsigned long long result = strtoull(text, &end, base);
    if (errno || *end || result > maximum)
        return false;
    *value = result;
    return true;
}

static bool read_prefix(char *text, struct in6_addr *address, unsigned *length)
{
    char *slash = strchr(text, '/');
    if (!slash)
        return false;
    *slash++ = '\0';
    uint64_t bits;
    if (!number(slash, 10, 128, &bits) || bits == 0 ||
        inet_pton(AF_INET6, text, address) != 1 ||
        IN6_IS_ADDR_MULTICAST(address) || IN6_IS_ADDR_LINKLOCAL(address) ||
        IN6_IS_ADDR_UNSPECIFIED(address) || IN6_IS_ADDR_LOOPBACK(address))
        return false;
    for (unsigned bit = bits; bit < 128; ++bit) {
        if (address->s6_addr[bit / 8] & (0x80 >> (bit % 8)))
            return false;
    }
    *length = bits;
    return true;
}

static bool prefix_matches(const struct in6_addr *a, const struct in6_addr *b,
                           unsigned bits)
{
    if (memcmp(a, b, bits / 8))
        return false;
    return bits % 8 == 0 ||
        !((a->s6_addr[bits / 8] ^ b->s6_addr[bits / 8]) & (0xff << (8 - bits % 8)));
}

static bool make_lan_prefix(const struct lease *lease, uint64_t subnet,
                            struct in6_addr *network)
{
    if (lease->length > 64 || subnet >= (UINT64_C(1) << (64 - lease->length)))
        return false;
    *network = lease->prefix;
    for (unsigned i = 0; i < 8; ++i)
        network->s6_addr[7 - i] |= (uint8_t)(subnet >> (8 * i));
    return !lease->excluded_length ||
        !prefix_matches(network, &lease->excluded, bounded(64, lease->excluded_length));
}

static bool parse_lease(char *entry, uint64_t now, struct lease *lease)
{
    char *prefix = strsep(&entry, ",");
    char *preferred = strsep(&entry, ",");
    char *valid = strsep(&entry, ",");
    uint64_t preferred_seconds, valid_seconds;
    if (!preferred || !valid || !read_prefix(prefix, &lease->prefix, &lease->length) ||
        !number(preferred, 10, UINT32_MAX, &preferred_seconds) ||
        !number(valid, 10, UINT32_MAX, &valid_seconds) ||
        preferred_seconds > valid_seconds)
        return false;

    /* Remaining lifetimes provided by odhcp6c are relative to this hook. */
    lease->preferred_until = now + 1000 * preferred_seconds;
    lease->valid_until = now + 1000 * valid_seconds;
    char *attribute;
    while ((attribute = strsep(&entry, ",")) != NULL) {
        if (!strncmp(attribute, "excluded=", 9) &&
            (!read_prefix(attribute + 9, &lease->excluded, &lease->excluded_length) ||
             lease->excluded_length <= lease->length ||
             !prefix_matches(&lease->prefix, &lease->excluded, lease->length)))
            return false;
    }
    return true;
}

static int send_update(int fd, const char *event)
{
    struct update update = {0};
    if (!strcmp(event, "bound") || !strcmp(event, "updated") || !strcmp(event, "rebound")) {
        const char *prefixes = getenv("PREFIXES");
        char *copy = strdup(prefixes ? prefixes : "");
        if (!copy)
            return 1;
        char *save = NULL;
        uint64_t now = now_ms();
        for (char *item = strtok_r(copy, " \t", &save); item; item = strtok_r(NULL, " \t", &save)) {
            if (update.count == MAX_LEASES || !parse_lease(item, now, &update.leases[update.count])) {
                fprintf(stderr, "pd-lan: invalid or excessive delegated prefixes\n");
                free(copy);
                return 1;
            }
            ++update.count;
        }
        free(copy);
    } else if (strcmp(event, "unbound") && strcmp(event, "stopped")) {
        return 0;
    }
    ssize_t size;
    do {
        size = write(fd, &update, sizeof(update));
    } while (size < 0 && errno == EINTR);
    return size != sizeof(update);
}

/* The musl sysroot omits Linux UAPI headers. These are the netlink wire fields. */
enum {
    NL_ERROR = 2, NL_REQUEST = 1, NL_ACK = 4, NL_CREATE = 0x400,
    NL_EXCL = 0x200, NL_REPLACE = 0x100, NL_NEWADDR = 20, NL_DELADDR = 21,
    NL_NEWROUTE = 24, NL_GETROUTE = 26, NL_IFA_LOCAL = 2, NL_IFA_CACHEINFO = 6,
    NL_RTA_OIF = 4, NL_RTN_UNICAST = 1, NL_MAIN_TABLE = 254,
};

struct nl_address {
    uint16_t family, pad;
    uint32_t pid, groups;
};

struct nl_header {
    uint32_t length;
    uint16_t type, flags;
    uint32_t seq, pid;
};

struct nl_addr {
    uint8_t family, prefix, flags, scope;
    uint32_t index;
};

struct nl_route {
    uint8_t family, dst_len, src_len, tos, table, protocol, scope, type;
    uint32_t flags;
};

struct nl_attr {
    uint16_t length, type;
};

static ssize_t request(int fd, struct nl_header *header, void *reply, size_t capacity)
{
    static uint32_t sequence;
    header->seq = ++sequence;
    if (send(fd, header, header->length, 0) != header->length)
        return -1;
    struct pollfd poll_fd = {fd, POLLIN, 0};
    int ready = poll(&poll_fd, 1, 500);
    if (ready <= 0) {
        if (ready == 0)
            errno = ETIMEDOUT;
        return -1;
    }
    ssize_t size = recv(fd, reply, capacity, 0);
    if (size < 0)
        return -1;
    struct nl_header response;
    if (size < (ssize_t)sizeof(response))
        goto invalid;
    memcpy(&response, reply, sizeof(response));
    if (response.seq != header->seq || response.length > (size_t)size ||
        response.length < sizeof(response))
        goto invalid;
    if (response.type == NL_ERROR) {
        int error;
        if (response.length < sizeof(response) + sizeof(error))
            goto invalid;
        memcpy(&error, (uint8_t *)reply + sizeof(response), sizeof(error));
        if (error == 0)
            return 0;
        errno = -error;
        return -1;
    }
    return response.length;

invalid:
    errno = EPROTO;
    return -1;
}

static int configure_address(int fd, const struct lan *lan, const struct prefix *prefix,
                             bool add, uint32_t preferred, uint32_t valid)
{
    struct {
        struct nl_header header;
        struct nl_addr address;
        struct nl_attr local;
        struct in6_addr value;
        struct nl_attr cache;
        uint32_t lifetime[4];
    } message = {
        .header = {.type = add ? NL_NEWADDR : NL_DELADDR, .flags = NL_REQUEST | NL_ACK},
        .address = {.family = AF_INET6, .prefix = 64, .index = lan->index},
        .local = {20, NL_IFA_LOCAL},
        .value = prefix->network,
        .cache = {20, NL_IFA_CACHEINFO},
        .lifetime = {preferred, valid},
    };
    message.value.s6_addr[15] = 1;
    message.header.length = add ? sizeof(message) : sizeof(message) - 20;
    if (add)
        message.header.flags |= NL_CREATE | (prefix->installed ? NL_REPLACE : NL_EXCL);
    uint8_t reply[128];
    return request(fd, &message.header, reply, sizeof(reply)) == 0 ? 0 : -1;
}

static bool wan_default_route(int fd, unsigned wan)
{
    struct {
        struct nl_header header;
        struct nl_route route;
    } message = {
        .header = {.length = sizeof(message), .type = NL_GETROUTE, .flags = NL_REQUEST},
        .route = {.family = AF_INET6},
    };
    uint8_t reply[256];
    ssize_t size = request(fd, &message.header, reply, sizeof(reply));
    size_t offset = sizeof(struct nl_header) + sizeof(struct nl_route);
    if (size < (ssize_t)offset)
        return false;
    struct nl_header header;
    struct nl_route route;
    memcpy(&header, reply, sizeof(header));
    memcpy(&route, reply + sizeof(header), sizeof(route));
    if (header.type != NL_NEWROUTE || route.family != AF_INET6 || route.dst_len ||
        route.table != NL_MAIN_TABLE || route.type != NL_RTN_UNICAST)
        return false;

    while (offset + sizeof(struct nl_attr) <= (size_t)size) {
        struct nl_attr attr;
        memcpy(&attr, reply + offset, sizeof(attr));
        if (attr.length < sizeof(attr) || offset + attr.length > (size_t)size)
            return false;
        if (attr.type == NL_RTA_OIF && attr.length == sizeof(attr) + sizeof(uint32_t)) {
            uint32_t index;
            memcpy(&index, reply + offset + sizeof(attr), sizeof(index));
            return index == wan;
        }
        offset += (attr.length + 3) & ~3u;
    }
    return false;
}

static void schedule_ra(struct lan *lan, uint64_t when)
{
    uint64_t earliest = lan->last_ra ? lan->last_ra + RA_MIN_SPACING : 0;
    if (when < earliest)
        when = earliest;
    if (!lan->next_ra || when < lan->next_ra)
        lan->next_ra = when;
}

static void withdraw(int rtnl, struct lan *lan, struct prefix *prefix, uint64_t now)
{
    if (configure_address(rtnl, lan, prefix, false, 0, 0) < 0 &&
        errno != EADDRNOTAVAIL && errno != ENOENT) {
        perror("pd-lan: remove LAN address");
        /* Keep ownership and retry removal while advertising its withdrawal. */
    } else {
        prefix->installed = false;
    }
    if (!prefix->withdraw_until) {
        uint32_t valid = bounded(remaining(prefix->valid_until, now), MAX_PIO_VALID);
        prefix->withdraw_until = now + (valid > 12 ? valid : 12) * UINT64_C(1000);
        schedule_ra(lan, now);
    }
}

static void reconcile(int rtnl, struct lan *lan, const struct update *leases, uint64_t now)
{
    for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
        struct prefix *prefix = &lan->prefixes[i];
        if (prefix->withdraw_until && prefix->withdraw_until <= now && !prefix->installed)
            memset(prefix, 0, sizeof(*prefix));
        prefix->wanted = false;
    }
    for (unsigned i = 0; i < leases->count; ++i) {
        const struct lease *lease = &leases->leases[i];
        now = now_ms();
        uint32_t valid = remaining(lease->valid_until, now);
        struct in6_addr network;
        if (!valid || !make_lan_prefix(lease, lan->subnet, &network))
            continue;
        struct prefix *entry = NULL, *reusable = NULL;
        for (unsigned j = 0; j < MAX_PREFIXES; ++j) {
            struct prefix *candidate = &lan->prefixes[j];
            if ((candidate->installed || candidate->withdraw_until) &&
                !memcmp(&candidate->network, &network, sizeof(network))) {
                entry = candidate;
                break;
            }
            if (!candidate->installed && (!reusable ||
                candidate->withdraw_until < reusable->withdraw_until))
                reusable = candidate;
        }
        if (!entry)
            entry = reusable;
        if (!entry || (entry->wanted && entry->valid_until >= lease->valid_until))
            continue;
        if (entry->installed && !entry->withdraw_until &&
            entry->valid_until == lease->valid_until &&
            entry->preferred_until == lease->preferred_until) {
            entry->wanted = true;
            continue;
        }
        struct prefix next = {
            .network = network, .preferred_until = lease->preferred_until,
            .valid_until = lease->valid_until, .installed = entry->installed, .wanted = true,
        };
        if (configure_address(rtnl, lan, &next, true,
                              remaining(next.preferred_until, now), valid) < 0) {
            perror("pd-lan: configure LAN address");
            if (entry->installed && !entry->withdraw_until)
                entry->wanted = true;
            continue;
        }
        next.installed = true;
        *entry = next;
        schedule_ra(lan, now);
    }
    for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
        struct prefix *entry = &lan->prefixes[i];
        if (entry->installed && !entry->wanted)
            withdraw(rtnl, lan, entry, now);
    }
}

static int open_lan(struct lan *lan, int ioctl_fd)
{
    struct ifreq request = {0};
    strcpy(request.ifr_name, lan->name);
    if (ioctl(ioctl_fd, SIOCGIFHWADDR, &request) < 0)
        return -1;
    if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    memcpy(lan->mac, request.ifr_hwaddr.sa_data, sizeof(lan->mac));
    int fd = socket(AF_INET6, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (fd < 0)
        return -1;
    struct icmp6_filter filter;
    ICMP6_FILTER_SETBLOCKALL(&filter);
    ICMP6_FILTER_SETPASS(ND_ROUTER_SOLICIT, &filter);
    struct ipv6_mreq group = {.ipv6mr_interface = lan->index};
    inet_pton(AF_INET6, "ff02::2", &group.ipv6mr_multiaddr);
    int one = 1, hops = 255;
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, lan->name, strlen(lan->name)) < 0 ||
        setsockopt(fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter, sizeof(filter)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group, sizeof(group)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) < 0 ||
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof(one)) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static bool solicitation(int fd)
{
    uint8_t packet[1500];
    union {
        struct cmsghdr align;
        uint8_t data[CMSG_SPACE(sizeof(int))];
    } control;
    struct sockaddr_in6 source;
    struct iovec iov = {packet, sizeof(packet)};
    struct msghdr msg = {
        .msg_name = &source, .msg_namelen = sizeof(source),
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.data, .msg_controllen = sizeof(control.data),
    };
    ssize_t size = recvmsg(fd, &msg, 0);
    if (size < 8 || msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
        packet[0] != ND_ROUTER_SOLICIT || packet[1] ||
        IN6_IS_ADDR_MULTICAST(&source.sin6_addr))
        return false;
    for (size_t i = 8; i < (size_t)size;) {
        if (i + 2 > (size_t)size || !packet[i + 1] || i + 8 * packet[i + 1] > (size_t)size)
            return false;
        if (packet[i] == ND_OPT_SOURCE_LINKADDR && IN6_IS_ADDR_UNSPECIFIED(&source.sin6_addr))
            return false;
        i += 8 * packet[i + 1];
    }
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            int hop;
            memcpy(&hop, CMSG_DATA(cmsg), sizeof(hop));
            return hop == 255;
        }
    }
    return false;
}

static void advertise(struct lan *lan, bool have_default, uint64_t now, bool final)
{
    struct {
        struct nd_router_advert header;
        uint8_t link[8];
        struct nd_opt_prefix_info prefixes[MAX_PREFIXES];
    } ra = {
        .header = {.nd_ra_type = ND_ROUTER_ADVERT, .nd_ra_curhoplimit = 64},
        .link = {ND_OPT_SOURCE_LINKADDR, 1},
    };
    memcpy(ra.link + 2, lan->mac, sizeof(lan->mac));
    unsigned count = 0;
    uint32_t longest = 0, interval = 12000;
    for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
        const struct prefix *entry = &lan->prefixes[i];
        if (!entry->installed && !entry->withdraw_until)
            continue;
        uint32_t valid = final || entry->withdraw_until ? 0 :
                         bounded(remaining(entry->valid_until, now), MAX_PIO_VALID);
        uint32_t preferred = bounded(valid, bounded(remaining(entry->preferred_until, now),
                                                    MAX_PIO_PREFERRED));
        if (valid) {
            interval = bounded(interval, valid * 1000 / 3);
            if (valid > longest)
                longest = valid;
        }
        ra.prefixes[count++] = (struct nd_opt_prefix_info){
            .nd_opt_pi_type = ND_OPT_PREFIX_INFORMATION, .nd_opt_pi_len = 4,
            .nd_opt_pi_prefix_len = 64, .nd_opt_pi_flags_reserved = ND_OPT_PI_FLAG_ONLINK | ND_OPT_PI_FLAG_AUTO,
            .nd_opt_pi_valid_time = htonl(valid), .nd_opt_pi_preferred_time = htonl(preferred),
            .nd_opt_pi_prefix = entry->network,
        };
    }
    lan->next_ra = 0;
    if (!count)
        return;
    if (have_default && longest >= 4)
        ra.header.nd_ra_router_lifetime = htons(bounded(longest, RA_ROUTER_LIFETIME));
    struct sockaddr_in6 target = {.sin6_family = AF_INET6, .sin6_scope_id = lan->index};
    inet_pton(AF_INET6, "ff02::1", &target.sin6_addr);
    size_t length = sizeof(ra.header) + sizeof(ra.link) + count * sizeof(ra.prefixes[0]);
    if (sendto(lan->fd, &ra, length, 0, (void *)&target, sizeof(target)) != (ssize_t)length) {
        if (errno != ENETDOWN && errno != EADDRNOTAVAIL && errno != ENETUNREACH)
            perror("pd-lan: send RA");
        lan->next_ra = now + 1000;
        return;
    }
    lan->last_ra = now;
    if (interval < 4000)
        interval = 4000;
    uint32_t minimum = interval * 3 / 4;
    lan->next_ra = now + minimum + rand_r(&random_seed) % (interval - minimum + 1);
}

static bool parse_lan(struct lan *lan, const char *config)
{
    const char *separator = strchr(config, ':');
    size_t length = separator ? (size_t)(separator - config) : strlen(config);
    if (!length || length >= sizeof(lan->name))
        return false;
    memcpy(lan->name, config, length);
    lan->index = if_nametoindex(lan->name);
    return lan->index && (!separator || number(separator + 1, 0, UINT64_MAX, &lan->subnet));
}

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static pid_t start_client(int pipe_fd[2], const char *client, const char *wan,
                          const char *length)
{
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    close(pipe_fd[0]);
    char fd[16];
    snprintf(fd, sizeof(fd), "%d", pipe_fd[1]);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0L, 0L, 0L) < 0 || getppid() != parent ||
        fcntl(pipe_fd[1], F_SETFD, 0) < 0 || setenv("A20_PD_EVENTS", fd, 1) < 0) {
        perror("pd-lan: set up odhcp6c child");
        _exit(126);
    }
    execl(client, client, "-e", "-k", "-F", "-N", "none", "-P", length,
          "-s", "/bin/pd-lan", wan, (char *)NULL);
    perror("pd-lan: exec odhcp6c");
    _exit(127);
}

static int run(int rtnl, struct lan *lans, unsigned count, unsigned wan,
               int event_fd, pid_t client)
{
    struct pollfd fds[MAX_LANS + 1] = {{event_fd, POLLIN, 0}};
    for (unsigned i = 0; i < count; ++i)
        fds[i + 1] = (struct pollfd){lans[i].fd, POLLIN, 0};
    struct update leases = {0};
    bool have_default = false;
    uint64_t next_check = 0;
    int rc = 0;
    while (!stopping) {
        uint64_t now = now_ms();
        if (now >= next_check) {
            bool available = wan_default_route(rtnl, wan);
            now = now_ms();
            for (unsigned i = 0; i < count; ++i) {
                reconcile(rtnl, &lans[i], &leases, now);
                if (available != have_default)
                    schedule_ra(&lans[i], now);
            }
            have_default = available;
            now = now_ms();
            next_check = now + 1000;
        }
        uint64_t wake = next_check;
        for (unsigned i = 0; i < count; ++i) {
            if (lans[i].next_ra && lans[i].next_ra <= now)
                advertise(&lans[i], have_default, now, false);
            if (lans[i].next_ra && lans[i].next_ra < wake)
                wake = lans[i].next_ra;
        }
        int ready = poll(fds, count + 1, wake > now ? (int)(wake - now) : 0);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            rc = 1;
            break;
        }
        if (fds[0].revents & POLLIN) {
            struct update next;
            if (read(event_fd, &next, sizeof(next)) != sizeof(next) || next.count > MAX_LEASES) {
                rc = 1;
                break;
            }
            leases = next;
            next_check = 0;
        } else if (fds[0].revents & (POLLHUP | POLLERR)) {
            rc = 1;
            break;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (fds[i + 1].revents & POLLIN && solicitation(lans[i].fd))
                schedule_ra(&lans[i], now_ms() + rand_r(&random_seed) % 501);
        }
    }

    const struct update empty = {0};
    for (unsigned i = 0; i < count; ++i)
        reconcile(rtnl, &lans[i], &empty, now_ms());
    for (unsigned i = 0; i < count; ++i) {
        uint64_t now = now_ms();
        uint64_t earliest = lans[i].last_ra + RA_MIN_SPACING;
        if (now < earliest) {
            struct timespec delay = {(earliest - now) / 1000,
                                     (earliest - now) % 1000 * 1000000};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
                ;
        }
        advertise(&lans[i], false, now_ms(), true);
    }
    kill(client, SIGTERM);
    close(event_fd);
    while (waitpid(client, NULL, 0) < 0 && errno == EINTR)
        ;
    return rc;
}

int main(int argc, char **argv)
{
    uint64_t event_fd, length;
    const char *event_pipe = getenv("A20_PD_EVENTS");
    if (argc == 3 && event_pipe && number(event_pipe, 10, INT_MAX, &event_fd))
        return send_update(event_fd, argv[2]);

    unsigned wan = argc >= 5 ? if_nametoindex(argv[2]) : 0;
    if (!wan || argc > MAX_LANS + 4 || !number(argv[3], 10, 64, &length) || length == 0) {
        fprintf(stderr, "usage: pd-lan ODHCP6C WAN PREFIX-LENGTH LAN[:SUBNET-ID] ...\n");
        return 2;
    }
    unsigned count = argc - 4;
    struct lan lans[MAX_LANS] = {0};
    for (unsigned i = 0; i < count; ++i) {
        if (!parse_lan(&lans[i], argv[4 + i]) || lans[i].index == wan ||
            lans[i].subnet >= (UINT64_C(1) << (64 - length))) {
            fprintf(stderr, "pd-lan: invalid LAN %s\n", argv[4 + i]);
            return 2;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (lans[i].index == lans[j].index || lans[i].subnet == lans[j].subnet) {
                fprintf(stderr, "pd-lan: LAN interfaces and subnet IDs must be unique\n");
                return 2;
            }
        }
    }
    int rtnl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct nl_address kernel = {.family = AF_NETLINK};
    if (rtnl < 0 || connect(rtnl, (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        perror("pd-lan: connect rtnetlink");
        return 1;
    }
    int ioctl_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ioctl_fd < 0) {
        perror("pd-lan: open interface control");
        return 1;
    }
    for (unsigned i = 0; i < count; ++i) {
        lans[i].fd = open_lan(&lans[i], ioctl_fd);
        if (lans[i].fd < 0) {
            perror(lans[i].name);
            return 1;
        }
    }
    close(ioctl_fd);

    int events[2];
    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        pipe2(events, O_CLOEXEC) < 0) {
        perror("pd-lan: set up lease events");
        return 1;
    }
    pid_t client = start_client(events, argv[1], argv[2], argv[3]);
    if (client < 0) {
        perror("pd-lan: start odhcp6c");
        return 1;
    }
    close(events[1]);
    fprintf(stderr, "pd-lan: running on %u LAN interface(s) for %s\n", count, argv[2]);
    random_seed = now_ms() ^ getpid();
    return run(rtnl, lans, count, wan, events[0], client);
}
