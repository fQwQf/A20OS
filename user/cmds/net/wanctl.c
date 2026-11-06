/* Supervise one Ethernet IPv4 WAN. BusyBox owns DHCP; wanctl owns the
 * resulting address, default route and per-WAN resolver file. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
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

enum { DNS_LIMIT = 4, RETRY_MS = 1000, POLL_MS = 250 };
enum { RTM_NEWADDR = 20, RTM_DELADDR = 21, RTM_NEWROUTE = 24, RTM_DELROUTE = 25,
       NLMSG_ERROR = 2, NLM_F_REQUEST = 1, NLM_F_ACK = 4,
       NLM_F_EXCL = 0x200, NLM_F_CREATE = 0x400,
       IFA_LOCAL = 2, RTA_OIF = 4, RTA_GATEWAY = 5 };

struct nl_header {
    uint32_t length;
    uint16_t type, flags;
    uint32_t sequence, pid;
};
struct nl_attr {
    uint16_t length, type;
    uint32_t value;
};
struct lease {
    uint32_t address, gateway, dns[DNS_LIMIT], seconds;
    unsigned prefix;
};
struct config {
    char interface[IFNAMSIZ], busybox[PATH_MAX], resolver[PATH_MAX];
    bool dhcp;
    struct lease fixed;
};
struct installed {
    struct lease lease;
    unsigned index;
    bool address, route, dns;
    struct stat dns_owner;
    uint64_t expires, renew_at;
};
struct client {
    pid_t pid;
    int events;
    uint64_t retry_at, kill_at;
};
static volatile sig_atomic_t stopping;

static void stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static uint64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (unsigned)now.tv_nsec / 1000000;
}

static bool number(const char *text, unsigned min, unsigned max, unsigned *out)
{
    char *end;
    if (!text || !isdigit((unsigned char)*text))
        return false;
    errno = 0;
    unsigned long result = strtoul(text, &end, 10);
    if (errno || *end || result < min || result > max)
        return false;
    *out = (unsigned)result;
    return true;
}

static bool address4(const char *text, uint32_t *out)
{
    if (!text || inet_pton(AF_INET, text, out) != 1)
        return false;
    unsigned first = ntohl(*out) >> 24;
    return first != 0 && first != 127 && first < 224;
}

static bool addresses4(const char *text, uint32_t *out, size_t capacity)
{
    size_t count = 0;
    while (text && *text) {
        text += strspn(text, " \t\n");
        size_t length = strcspn(text, " \t\n");
        if (!length)
            break;
        char word[INET_ADDRSTRLEN];
        uint32_t parsed;
        if (length >= sizeof(word))
            return false;
        memcpy(word, text, length);
        word[length] = 0;
        if (!address4(word, &parsed))
            return false;
        if (count < capacity)
            out[count++] = parsed;
        text += length;
    }
    return true;
}

static uint32_t mask4(unsigned prefix)
{
    return prefix ? UINT32_MAX << (32 - prefix) : 0;
}

static bool validate_lease(const struct lease *lease)
{
    uint32_t mask = mask4(lease->prefix);
    uint32_t address = ntohl(lease->address), gateway = ntohl(lease->gateway);
    if (lease->prefix < 31 && ((address & ~mask) == 0 || (address & ~mask) == ~mask))
        return false;
    return !gateway || (gateway != address && !((address ^ gateway) & mask) &&
           (lease->prefix >= 31 || ((gateway & ~mask) != 0 && (gateway & ~mask) != ~mask)));
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text))
        ++text;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1]))
        text[--n] = 0;
    return text;
}

static bool copy_path(char out[PATH_MAX], const char *value)
{
    if (value[0] != '/' || strlen(value) >= PATH_MAX)
        return false;
    strcpy(out, value);
    return true;
}

static bool read_config(const char *path, struct config *cfg)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return false;
    }
    enum { IFACE = 1, MODE = 2, ADDRESS = 4, GATEWAY = 8, DNS = 16, CLIENT = 32, RESOLVER = 64 };
    strcpy(cfg->resolver, "/tmp/wan-resolv.conf");
    unsigned seen = 0, line_number = 0;
    bool valid = true;
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), file)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(file)) {
            valid = false;
            break;
        }
        char *key = trim(line);
        if (!*key || *key == '#')
            continue;
        char *value = strchr(key, '=');
        if (!value) {
            valid = false;
            break;
        }
        *value++ = 0;
        key = trim(key);
        value = trim(value);
        unsigned field = 0;
        if (!strcmp(key, "interface")) {
            field = IFACE;
            valid = *value && strlen(value) < IFNAMSIZ && !strpbrk(value, " /\t");
            if (valid) strcpy(cfg->interface, value);
        } else if (!strcmp(key, "mode")) {
            field = MODE;
            cfg->dhcp = !strcmp(value, "dhcp");
            valid = cfg->dhcp || !strcmp(value, "static");
        } else if (!strcmp(key, "address")) {
            field = ADDRESS;
            char *prefix = strchr(value, '/');
            if (prefix) *prefix++ = 0;
            valid = address4(value, &cfg->fixed.address) && number(prefix, 1, 32, &cfg->fixed.prefix);
        } else if (!strcmp(key, "gateway")) {
            field = GATEWAY;
            valid = address4(value, &cfg->fixed.gateway);
        } else if (!strcmp(key, "dns")) {
            field = DNS;
            valid = addresses4(value, cfg->fixed.dns, DNS_LIMIT);
        } else if (!strcmp(key, "busybox")) {
            field = CLIENT;
            valid = copy_path(cfg->busybox, value);
        } else if (!strcmp(key, "resolver")) {
            field = RESOLVER;
            valid = copy_path(cfg->resolver, value);
        }
        if (!field || (seen & field) || !valid) {
            valid = false;
            break;
        }
        seen |= field;
    }
    valid = valid && !ferror(file);
    fclose(file);
    valid = valid && (seen & (IFACE | MODE)) == (IFACE | MODE) &&
            (cfg->dhcp ? (seen & CLIENT) && !(seen & (ADDRESS | GATEWAY | DNS)) :
             (seen & ADDRESS) && !(seen & CLIENT) && validate_lease(&cfg->fixed));
    if (!valid)
        fprintf(stderr, "wanctl: invalid configuration in %s near line %u\n", path, line_number);
    return valid;
}

static int dhcp_event(const char *kind, const char *fd_text)
{
    unsigned fd;
    if (!number(fd_text, 3, INT_MAX, &fd))
        return 2;
    struct lease lease = {0};
    if (!strcmp(kind, "bound") || !strcmp(kind, "renew")) {
        uint32_t subnet;
        if (!address4(getenv("ip"), &lease.address) ||
            !getenv("subnet") || inet_pton(AF_INET, getenv("subnet"), &subnet) != 1 ||
            !addresses4(getenv("router"), &lease.gateway, 1) ||
            !addresses4(getenv("dns"), lease.dns, DNS_LIMIT) ||
            !number(getenv("lease"), 1, UINT32_MAX, &lease.seconds))
            return 2;
        while (lease.prefix < 32 && (ntohl(subnet) & (UINT32_C(1) << (31 - lease.prefix))))
            ++lease.prefix;
        if (!lease.prefix || mask4(lease.prefix) != ntohl(subnet) || !validate_lease(&lease))
            return 2;
    } else if (strcmp(kind, "deconfig") && strcmp(kind, "nak") && strcmp(kind, "leasefail")) {
        return 0;
    }
    _Static_assert(sizeof(lease) <= 512, "DHCP event fits a single pipe write");
    return write((int)fd, &lease, sizeof(lease)) == sizeof(lease) ? 0 : 1;
}

static int netlink_ack(const void *request, size_t length)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct {
        uint16_t family, pad;
        uint32_t pid, groups;
    } local = {.family = AF_NETLINK};
    int error = 0;
    ssize_t sent = bind(fd, (void *)&local, sizeof(local)) < 0 ?
                   -1 : send(fd, request, length, 0);
    if (sent != (ssize_t)length) {
        error = sent < 0 ? errno : EIO;
    } else {
        struct pollfd wait = {.fd = fd, .events = POLLIN};
        int ready = poll(&wait, 1, 2000);
        if (ready <= 0) {
            error = ready < 0 ? errno : ETIMEDOUT;
        } else {
            struct { struct nl_header header; int32_t error; char padding[108]; } reply;
            ssize_t count = recv(fd, &reply, sizeof(reply), 0);
            if (count < 0)
                error = errno;
            else if (count < 20 || reply.header.type != NLMSG_ERROR ||
                     reply.header.sequence != 1 || reply.error > 0)
                error = EPROTO;
            else
                error = -reply.error;
        }
    }
    close(fd);
    errno = error;
    return error ? -1 : 0;
}

static int change_address(unsigned index, const struct lease *lease, bool add)
{
    struct {
        struct nl_header nl;
        uint8_t family, prefix, flags, scope;
        uint32_t index;
        struct nl_attr local;
    } request = {
        .nl = {sizeof(request), add ? RTM_NEWADDR : RTM_DELADDR,
               NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_EXCL : 0), 1, 0},
        .family = AF_INET, .prefix = lease->prefix, .index = index,
        .local = {8, IFA_LOCAL, lease->address},
    };
    return netlink_ack(&request, sizeof(request));
}

static int change_route(unsigned index, uint32_t gateway, bool add)
{
    struct {
        struct nl_header nl;
        uint8_t family, dst, src, tos, table, protocol, scope, kind;
        uint32_t flags;
        struct nl_attr device, gateway;
    } request = {
        .nl = {sizeof(request), add ? RTM_NEWROUTE : RTM_DELROUTE,
               NLM_F_REQUEST | NLM_F_ACK | (add ? NLM_F_CREATE | NLM_F_EXCL : 0), 1, 0},
        .family = AF_INET, .table = 254, .protocol = 16, .kind = 1,
        .device = {8, RTA_OIF, index}, .gateway = {8, RTA_GATEWAY, gateway},
    };
    return netlink_ack(&request, sizeof(request));
}

static bool owns_dns(const struct config *cfg, const struct installed *state)
{
    struct stat file;
    return state->dns && lstat(cfg->resolver, &file) == 0 &&
           file.st_dev == state->dns_owner.st_dev && file.st_ino == state->dns_owner.st_ino;
}

static int publish_dns(const struct config *cfg, struct installed *state, const uint32_t *servers)
{
    if (!servers[0]) {
        if (owns_dns(cfg, state) && unlink(cfg->resolver) < 0 && errno != ENOENT)
            return -1;
        state->dns = false;
        return 0;
    }
    char temporary[PATH_MAX + 16];
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", cfg->resolver);
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        int error = errno;
        close(fd);
        unlink(temporary);
        errno = error;
        return -1;
    }
    int error = 0;
    struct stat owner;
    for (unsigned i = 0; i < DNS_LIMIT && servers[i]; ++i) {
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &servers[i], ip, sizeof(ip)) || fprintf(file, "nameserver %s\n", ip) < 0) {
            error = errno ? errno : EIO;
            break;
        }
    }
    if (!error && (fchmod(fd, 0644) < 0 || fstat(fd, &owner) < 0))
        error = errno;
    if (fclose(file) < 0 && !error)
        error = errno;
    if (!error) {
        int rc = owns_dns(cfg, state) ? rename(temporary, cfg->resolver) :
                                      link(temporary, cfg->resolver);
        if (rc < 0)
            error = errno;
        else {
            state->dns = true;
            state->dns_owner = owner;
        }
    }
    unlink(temporary);
    errno = error;
    return error ? -1 : 0;
}

static bool clear_config(const struct config *cfg, struct installed *state)
{
    if (state->route && (change_route(state->index, state->lease.gateway, false) == 0 ||
                         errno == ESRCH || errno == ENOENT || errno == ENODEV))
        state->route = false;
    if (!state->route && state->address && (change_address(state->index, &state->lease, false) == 0 ||
                                            errno == EADDRNOTAVAIL || errno == ENODEV))
        state->address = false;
    const uint32_t empty[DNS_LIMIT] = {0};
    (void)publish_dns(cfg, state, empty);
    return !state->address && !state->route && !state->dns;
}

static int apply_config(const struct config *cfg, struct installed *state, unsigned index,
                        const struct lease *lease)
{
    if (!state->address || state->index != index || state->lease.address != lease->address ||
        state->lease.prefix != lease->prefix || state->lease.gateway != lease->gateway) {
        if (!clear_config(cfg, state)) {
            errno = EBUSY;
            return -1;
        }
        state->index = index;
        state->lease = *lease;
        if (change_address(index, lease, true) < 0)
            return -1;
        state->address = true;
        if (lease->gateway) {
            if (change_route(index, lease->gateway, true) < 0)
                goto rollback;
            state->route = true;
        }
    }
    if ((!owns_dns(cfg, state) || memcmp(state->lease.dns, lease->dns, sizeof(lease->dns))) &&
        publish_dns(cfg, state, lease->dns) < 0)
        goto rollback;
    state->lease = *lease;
    uint64_t now = now_ms();
    state->expires = lease->seconds ? now + (uint64_t)lease->seconds * 1000 : UINT64_MAX;
    /* BusyBox clamps leases below 30 seconds for its internal timers. Ask
     * it to renew at half the actual lease so expiry never races its T1. */
    state->renew_at = lease->seconds && lease->seconds < 30 ?
                      now + (uint64_t)lease->seconds * 500 : UINT64_MAX;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &lease->address, ip, sizeof(ip));
    printf("WANCTL: %s configured %s/%u\n", cfg->interface, ip, lease->prefix);
    return 0;

rollback: {
    int error = errno;
    (void)clear_config(cfg, state);
    errno = error;
    return -1;
}
}

static unsigned ready_interface(int ctl, const char *name)
{
    struct ifreq request = {0};
    strcpy(request.ifr_name, name);
    if (ioctl(ctl, SIOCGIFFLAGS, &request) < 0 ||
        (request.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
        return 0;
    if (ioctl(ctl, SIOCGIFINDEX, &request) < 0 || request.ifr_ifindex <= 0)
        return 0;
    return (unsigned)request.ifr_ifindex;
}

static int start_client(struct client *client, const struct config *cfg, const char *self)
{
    int pipe_fd[2];
    if (pipe2(pipe_fd, O_CLOEXEC) < 0)
        return -1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fd[0]);
        char fd[16];
        snprintf(fd, sizeof(fd), "%d", pipe_fd[1]);
        if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0L, 0L, 0L) < 0 || getppid() != parent ||
            fcntl(pipe_fd[1], F_SETFD, 0) < 0 || setenv("A20_WAN_EVENTS", fd, 1) < 0)
            _exit(126);
        execl(cfg->busybox, cfg->busybox, "udhcpc", "-f", "-i", cfg->interface,
              "-s", self, "-T", "2", "-t", "3", "-A", "2", (char *)NULL);
        perror("wanctl: exec BusyBox");
        _exit(127);
    }
    close(pipe_fd[1]);
    if (pid < 0) {
        close(pipe_fd[0]);
        return -1;
    }
    client->pid = pid;
    client->events = pipe_fd[0];
    client->kill_at = 0;
    printf("WANCTL: %s DHCP client pid=%ld\n", cfg->interface, (long)pid);
    return 0;
}

static void stop_client(struct client *client, uint64_t now)
{
    if (client->events >= 0) {
        close(client->events);
        client->events = -1;
    }
    if (client->pid > 0 && !client->kill_at) {
        kill(client->pid, SIGTERM);
        client->kill_at = now + 2000;
    }
}

static bool reap_client(struct client *client, uint64_t now)
{
    if (client->pid <= 0)
        return false;
    pid_t result = waitpid(client->pid, NULL, WNOHANG);
    if (result == 0) {
        if (client->kill_at && now >= client->kill_at)
            kill(client->pid, SIGKILL);
        return false;
    }
    if (result < 0 && errno != ECHILD)
        return false;
    client->pid = 0;
    client->retry_at = now + RETRY_MS;
    stop_client(client, now);
    return true;
}

static int supervise(const struct config *cfg, const char *self, int ctl)
{
    struct client client = {.events = -1};
    struct installed state = {0};
    unsigned previous = 0;
    bool withdraw = false;
    while (!stopping) {
        uint64_t now = now_ms();
        unsigned index = ready_interface(ctl, cfg->interface);
        if (index != previous) {
            if (previous)
                stop_client(&client, now);
            withdraw = true;
            previous = index;
            printf("WANCTL: %s carrier %s\n", cfg->interface, index ? "up" : "down");
        }
        if (reap_client(&client, now))
            withdraw = true;
        if (state.address && now >= state.expires) {
            stop_client(&client, now);
            withdraw = true;
        }
        if (withdraw)
            withdraw = !clear_config(cfg, &state);
        if (state.address && !withdraw && client.pid > 0 && !client.kill_at &&
            now >= state.renew_at) {
            state.renew_at = UINT64_MAX;
            kill(client.pid, SIGUSR1);
        }
        if (index && !withdraw && now >= client.retry_at) {
            if (cfg->dhcp && client.pid == 0) {
                if (start_client(&client, cfg, self) < 0) {
                    perror("wanctl: start DHCP");
                    client.retry_at = now + RETRY_MS;
                }
            } else if (!cfg->dhcp && !state.address && apply_config(cfg, &state, index, &cfg->fixed) < 0) {
                perror("wanctl: static configuration");
                withdraw = true;
                client.retry_at = now + RETRY_MS;
            }
        }
        struct pollfd event = {.fd = client.events, .events = POLLIN};
        if (poll(&event, 1, POLL_MS) <= 0 || !(event.revents & (POLLIN | POLLHUP)))
            continue;
        struct lease lease;
        ssize_t size = read(client.events, &lease, sizeof(lease));
        if (size < 0 && errno == EINTR)
            continue;
        if (size != sizeof(lease)) {
            stop_client(&client, now);
            withdraw = true;
        } else if (!lease.address || ready_interface(ctl, cfg->interface) != index || withdraw) {
            withdraw = true;
        } else if (apply_config(cfg, &state, index, &lease) < 0) {
            perror("wanctl: apply DHCP lease");
            stop_client(&client, now);
            withdraw = true;
        }
    }
    stop_client(&client, now_ms());
    bool cleared = clear_config(cfg, &state);
    while (client.pid > 0) {
        (void)reap_client(&client, now_ms());
        if (client.pid > 0)
            poll(NULL, 0, POLL_MS);
    }
    if (!cleared)
        cleared = clear_config(cfg, &state);
    return cleared ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *events = getenv("A20_WAN_EVENTS");
    if (argc == 2 && events)
        return dhcp_event(argv[1], events);
    if (argc != 2) {
        fprintf(stderr, "usage: wanctl CONFIG\n");
        return 2;
    }
    struct config cfg = {0};
    if (!read_config(argv[1], &cfg))
        return 2;
    const char *self = argv[0][0] == '/' ? argv[0] : "/bin/wanctl";
    int lock = open("/tmp/wanctl.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
        perror("wanctl: lock");
        return 1;
    }
    int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctl < 0) {
        perror("wanctl: control socket");
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction signal_action = {.sa_handler = stop};
    sigemptyset(&signal_action.sa_mask);
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);
    int status = supervise(&cfg, self, ctl);
    close(ctl);
    close(lock);
    return status;
}
