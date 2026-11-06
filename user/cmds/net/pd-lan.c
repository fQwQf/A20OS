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
#include <ifaddrs.h>
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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
    uint32_t advertised_valid;
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

struct journal {
    const char *path;
    int directory;
    bool dirty;
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

/* The version fixes the PIO A/L flags. Entries are keyed by LAN name and
 * Ethernet address so a state file cannot silently follow another link. */
static void lan_mac(const struct lan *lan, char text[13])
{
    snprintf(text, 13, "%02x%02x%02x%02x%02x%02x", lan->mac[0], lan->mac[1],
             lan->mac[2], lan->mac[3], lan->mac[4], lan->mac[5]);
}

static int journal_load(const struct journal *state, struct lan *lans, unsigned count)
{
    int fd = open(state->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat status;
    int error = 0;
    if (fstat(fd, &status) < 0)
        error = errno;
    else if (!S_ISREG(status.st_mode))
        error = EINVAL;
    if (error) {
        close(fd);
        errno = error;
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    char line[128];
    if (!fgets(line, sizeof(line), file) || strcmp(line, "PDLAN1\n"))
        goto invalid;
    uint64_t now = now_ms();
    while (fgets(line, sizeof(line), file)) {
        char name[IF_NAMESIZE], mac[13], address[INET6_ADDRSTRLEN + 4], hold[11], extra;
        struct in6_addr network;
        unsigned length;
        uint64_t duration;
        if (!strchr(line, '\n') ||
            sscanf(line, "%15s %12s %49s %10s %c", name, mac, address, hold, &extra) != 4 ||
            !read_prefix(address, &network, &length) || length != 64 ||
            !number(hold, 10, MAX_PIO_VALID, &duration) || !duration)
            goto invalid;
        struct lan *lan = NULL;
        for (unsigned i = 0; i < count; ++i) {
            if (strcmp(lans[i].name, name))
                continue;
            char expected[13];
            lan_mac(&lans[i], expected);
            if (!strcmp(expected, mac))
                lan = &lans[i];
            break;
        }
        if (!lan)
            goto invalid;
        struct prefix *empty = NULL;
        for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
            struct prefix *entry = &lan->prefixes[i];
            if (!entry->advertised_valid) {
                if (!empty)
                    empty = entry;
            } else if (!memcmp(&entry->network, &network, sizeof(network))) {
                goto invalid;
            }
        }
        if (!empty)
            goto invalid;
        /* Keep the entire previous lifetime without trusting the RTC. The
         * journal is a withdrawal record, never an upstream delegation. */
        *empty = (struct prefix){
            .network = network, .advertised_valid = (uint32_t)duration,
            .withdraw_until = now + duration * 1000, .installed = true,
        };
        schedule_ra(lan, now);
    }
    if (ferror(file))
        goto invalid;
    return fclose(file);

invalid:
    fclose(file);
    errno = EINVAL;
    return -1;
}

static int journal_open(struct journal *state, struct lan *lans, unsigned count)
{
    if (!state->path)
        return 0;
    size_t length = strlen(state->path);
    if (state->path[0] != '/' || length >= PATH_MAX - 16 || state->path[length - 1] == '/') {
        errno = EINVAL;
        return -1;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s.lock", state->path);
    int lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
        return -1;
    /* The process owns the open lock for its lifetime. */
    strcpy(path, state->path);
    char *slash = strrchr(path, '/');
    if (slash == path)
        ++slash;
    *slash = '\0';
    state->directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (state->directory < 0)
        return -1;
    return journal_load(state, lans, count);
}

static bool journal_save(struct journal *state, const struct lan *lans, unsigned count)
{
    if (!state->path)
        state->dirty = false;
    if (!state->dirty)
        return true;
    struct stat status;
    if (lstat(state->path, &status) == 0) {
        if (!S_ISREG(status.st_mode)) {
            errno = EINVAL;
            return false;
        }
    } else if (errno != ENOENT) {
        return false;
    }
    char temporary[PATH_MAX];
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", state->path);
    int fd = mkstemp(temporary);
    if (fd < 0)
        return false;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        int error = errno;
        close(fd);
        unlink(temporary);
        errno = error;
        return false;
    }
    bool ok = fputs("PDLAN1\n", file) != EOF;
    for (unsigned i = 0; ok && i < count; ++i) {
        char mac[13];
        lan_mac(&lans[i], mac);
        for (unsigned j = 0; ok && j < MAX_PREFIXES; ++j) {
            const struct prefix *entry = &lans[i].prefixes[j];
            if (!entry->advertised_valid)
                continue;
            char network[INET6_ADDRSTRLEN];
            ok = inet_ntop(AF_INET6, &entry->network, network, sizeof(network)) &&
                 fprintf(file, "%s %s %s/64 %u\n", lans[i].name, mac, network,
                         entry->advertised_valid) > 0;
        }
    }
    if (ok)
        ok = fflush(file) == 0 && fsync(fd) == 0;
    int error = ok ? 0 : errno;
    if (fclose(file) < 0 && !error)
        error = errno;
    if (!error && (rename(temporary, state->path) < 0 || fsync(state->directory) < 0))
        error = errno;
    if (error) {
        unlink(temporary);
        errno = error;
        return false;
    }
    state->dirty = false;
    return true;
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
        if (prefix->advertised_valid > valid)
            valid = prefix->advertised_valid;
        prefix->withdraw_until = now + (valid > 12 ? valid : 12) * UINT64_C(1000);
        schedule_ra(lan, now);
    }
}

static void reconcile(int rtnl, struct lan *lan, const struct update *leases, uint64_t now,
                      struct journal *journal)
{
    for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
        struct prefix *prefix = &lan->prefixes[i];
        if (prefix->withdraw_until && prefix->withdraw_until <= now && !prefix->installed) {
            journal->dirty |= prefix->advertised_valid != 0;
            memset(prefix, 0, sizeof(*prefix));
        }
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
            if (!candidate->installed && !candidate->withdraw_until && !reusable)
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
            .advertised_valid = entry->advertised_valid,
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

static void advertise(struct lan *lan, bool have_default, uint64_t now, bool final,
                      struct journal *journal, const struct lan *lans, unsigned lan_count)
{
    if (!final) {
        for (unsigned i = 0; i < MAX_PREFIXES; ++i) {
            struct prefix *entry = &lan->prefixes[i];
            if (!entry->installed || entry->withdraw_until)
                continue;
            uint32_t valid = bounded(remaining(entry->valid_until, now), MAX_PIO_VALID);
            if (valid > entry->advertised_valid) {
                entry->advertised_valid = valid;
                journal->dirty = true;
            }
        }
    }
    bool durable = journal_save(journal, lans, lan_count);
    if (!durable)
        perror("pd-lan: save prefix journal");
    now = now_ms();

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
        if (valid && !durable)
            continue;
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
    lan->next_ra = durable ? 0 : now + 1000;
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

struct client {
    const char *program, *wan, *length;
    pid_t pid;
    unsigned index, failures;
    int event_fd;
    uint64_t retry_at, kill_at;
};

static void close_events(struct client *client)
{
    if (client->event_fd >= 0) {
        close(client->event_fd);
        client->event_fd = -1;
    }
}

static void stop_client(struct client *client, uint64_t now)
{
    close_events(client);
    if (client->pid > 0 && !client->kill_at) {
        (void)kill(client->pid, SIGTERM);
        client->kill_at = now + 2000;
    }
}

static void reap_client(struct client *client, uint64_t now)
{
    if (client->pid <= 0)
        return;
    pid_t result = waitpid(client->pid, NULL, WNOHANG);
    if (result == client->pid || (result < 0 && errno == ECHILD)) {
        close_events(client);
        client->pid = 0;
        client->kill_at = 0;
        client->retry_at = now + (1000u << bounded(client->failures, 4));
        if (client->failures < 4)
            ++client->failures;
    } else if (client->kill_at && now >= client->kill_at) {
        (void)kill(client->pid, SIGKILL);
    }
}

static bool link_local_ready(const char *name)
{
    struct ifaddrs *addresses;
    if (getifaddrs(&addresses) < 0)
        return false;
    bool ready = false;
    for (struct ifaddrs *ifa = addresses; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET6 &&
            (ifa->ifa_flags & IFF_UP) && !strcmp(name, ifa->ifa_name)) {
            const struct sockaddr_in6 *addr = (const void *)ifa->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr)) {
                ready = true;
                break;
            }
        }
    }
    freeifaddrs(addresses);
    return ready;
}

static void launch_client(struct client *client, unsigned index, uint64_t now)
{
    int pipe_fd[2];
    if (pipe2(pipe_fd, O_CLOEXEC) < 0) {
        perror("pd-lan: open lease event pipe");
        client->retry_at = now + 1000;
        return;
    }
    pid_t pid = start_client(pipe_fd, client->program, client->wan, client->length);
    close(pipe_fd[1]);
    if (pid < 0) {
        perror("pd-lan: start odhcp6c");
        close(pipe_fd[0]);
        client->retry_at = now + 1000;
        return;
    }
    client->index = index;
    client->pid = pid;
    client->event_fd = pipe_fd[0];
    fprintf(stderr, "pd-lan: started DHCPv6 on %s (ifindex %u)\n", client->wan, index);
}

static unsigned wan_index(int ioctl_fd, const char *name)
{
    struct ifreq request = {0};
    strcpy(request.ifr_name, name);
    if (ioctl(ioctl_fd, SIOCGIFFLAGS, &request) < 0 ||
        (request.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
        return 0;
    return if_nametoindex(name);
}

static bool removed_wan(int events, unsigned index)
{
    enum { NL_DELLINK = 17 };
    uint8_t buffer[1024];
    bool removed = false;
    /* Bound the work so bursts of unrelated link events cannot delay leases. */
    for (unsigned i = 0; i < 16; ++i) {
        ssize_t size = recv(events, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (size <= 0)
            break;
        size_t offset = 0;
        while (offset + sizeof(struct nl_header) <= (size_t)size) {
            struct nl_header header;
            memcpy(&header, buffer + offset, sizeof(header));
            if (header.length < sizeof(header) || header.length > (size_t)size - offset)
                break;
            /* ifinfomsg carries ifi_index four bytes into its body. */
            if (index && header.type == NL_DELLINK && header.length >= sizeof(header) + 16) {
                uint32_t removed_index;
                memcpy(&removed_index, buffer + offset + sizeof(header) + 4,
                       sizeof(removed_index));
                removed |= index == removed_index;
            }
            offset += (header.length + 3) & ~3u;
        }
    }
    return removed;
}

static void lose_link(struct client *client, struct update *leases, uint64_t now)
{
    stop_client(client, now);
    client->index = 0;
    client->failures = 0;
    client->retry_at = now;
    leases->count = 0;
    fprintf(stderr, "pd-lan: WAN link %s removed or down\n", client->wan);
}

static int run(int rtnl, int ioctl_fd, int links, struct lan *lans,
               unsigned count, struct client *client, struct journal *journal)
{
    struct pollfd fds[MAX_LANS + 2] = {{-1, POLLIN, 0}, {links, POLLIN, 0}};
    for (unsigned i = 0; i < count; ++i)
        fds[i + 2] = (struct pollfd){lans[i].fd, POLLIN, 0};
    struct update leases = {0};
    bool have_default = false;
    uint64_t next_check = 0;
    int rc = 0;
    while (!stopping) {
        uint64_t now = now_ms();
        if (now >= next_check) {
            unsigned index = wan_index(ioctl_fd, client->wan);
            if (client->index && index != client->index)
                lose_link(client, &leases, now);
            reap_client(client, now);
            if (!client->pid && index && now >= client->retry_at && link_local_ready(client->wan))
                launch_client(client, index, now);
            bool available = index && wan_default_route(rtnl, index);
            now = now_ms();
            for (unsigned i = 0; i < count; ++i) {
                reconcile(rtnl, &lans[i], &leases, now, journal);
                if (available != have_default)
                    schedule_ra(&lans[i], now);
            }
            if (!journal_save(journal, lans, count))
                perror("pd-lan: prune prefix journal");
            have_default = available;
            now = now_ms();
            next_check = now + 1000;
        }
        uint64_t wake = next_check;
        for (unsigned i = 0; i < count; ++i) {
            if (lans[i].next_ra && lans[i].next_ra <= now)
                advertise(&lans[i], have_default, now, false, journal, lans, count);
            if (lans[i].next_ra && lans[i].next_ra < wake)
                wake = lans[i].next_ra;
        }
        fds[0].fd = client->event_fd;
        int ready = poll(fds, count + 2, wake > now ? (int)(wake - now) : 0);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            rc = 1;
            break;
        }
        if (fds[1].revents & POLLIN && removed_wan(links, client->index)) {
            lose_link(client, &leases, now_ms());
            next_check = 0;
        }
        if (client->event_fd >= 0 && fds[0].revents & POLLIN) {
            struct update next;
            if (read(client->event_fd, &next, sizeof(next)) == sizeof(next) &&
                next.count <= MAX_LEASES) {
                leases = next;
                if (leases.count)
                    client->failures = 0;
            } else {
                stop_client(client, now_ms());
            }
            next_check = 0;
        } else if (client->event_fd >= 0 && fds[0].revents & (POLLHUP | POLLERR)) {
            stop_client(client, now_ms());
            next_check = 0;
        }
        for (unsigned i = 0; i < count; ++i) {
            if (fds[i + 2].revents & POLLIN && solicitation(lans[i].fd))
                schedule_ra(&lans[i], now_ms() + rand_r(&random_seed) % 501);
        }
    }

    stop_client(client, now_ms());
    const struct update empty = {0};
    for (unsigned i = 0; i < count; ++i)
        reconcile(rtnl, &lans[i], &empty, now_ms(), journal);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t now = now_ms();
        uint64_t earliest = lans[i].last_ra + RA_MIN_SPACING;
        if (now < earliest) {
            struct timespec delay = {(earliest - now) / 1000,
                                     (earliest - now) % 1000 * 1000000};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
                ;
        }
        advertise(&lans[i], false, now_ms(), true, journal, lans, count);
    }
    while (client->pid > 0) {
        reap_client(client, now_ms());
        if (client->pid > 0)
            (void)poll(NULL, 0, 50);
    }
    return rc;
}

int main(int argc, char **argv)
{
    uint64_t event_fd, length;
    const char *event_pipe = getenv("A20_PD_EVENTS");
    if (argc == 3 && event_pipe && number(event_pipe, 10, INT_MAX, &event_fd))
        return send_update(event_fd, argv[2]);

    struct journal journal = {.directory = -1};
    if (argc >= 3 && !strcmp(argv[1], "--state")) {
        journal.path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 5 || argc > MAX_LANS + 4 || !argv[2][0] || strlen(argv[2]) >= IF_NAMESIZE ||
        !number(argv[3], 10, 64, &length) || length == 0) {
        fprintf(stderr, "usage: pd-lan [--state ABSOLUTE-PATH] ODHCP6C WAN PREFIX-LENGTH LAN[:SUBNET-ID] ...\n");
        return 2;
    }
    if (access(argv[1], X_OK) < 0) {
        perror("pd-lan: access odhcp6c");
        return 1;
    }
    unsigned count = argc - 4;
    struct lan lans[MAX_LANS] = {0};
    for (unsigned i = 0; i < count; ++i) {
        if (!parse_lan(&lans[i], argv[4 + i]) || !strcmp(lans[i].name, argv[2]) ||
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
    if (journal_open(&journal, lans, count) < 0) {
        perror("pd-lan: open prefix journal");
        return 1;
    }
    enum { NETLINK_LEVEL = 270, ADD_MEMBERSHIP = 1, LINK_GROUP = 1 };
    int links = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int group = LINK_GROUP;
    if (links < 0 || setsockopt(links, NETLINK_LEVEL, ADD_MEMBERSHIP, &group, sizeof(group)) < 0) {
        perror("pd-lan: subscribe to link changes");
        return 1;
    }

    struct sigaction action = {.sa_handler = stop};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("pd-lan: set up signals");
        return 1;
    }
    struct client client = {.program = argv[1], .wan = argv[2], .length = argv[3], .event_fd = -1};
    fprintf(stderr, "pd-lan: running on %u LAN interface(s) for %s\n", count, argv[2]);
    random_seed = now_ms() ^ getpid();
    return run(rtnl, ioctl_fd, links, lans, count, &client, &journal);
}
