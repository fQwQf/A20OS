/* Exercise the routed TUN path with a fixed PPPoE session on an isolated wire.
 * The peer supplies packets. This fixture does not negotiate a session. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define TUNSETIFF _IOW('T', 202, int)
#define IFF_TUN 1
#define IFF_NO_PI 0x1000
#define PPP_SESSION 0x8864
#define PPP_OVERHEAD 22
#define PPP_MTU 1492

static const unsigned char peer[6] = {2, 0, 0, 1, 0, 2};
static const char tun_name[] = "ppp-test";

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "TUN_ROUTER %s (errno=%d): FAIL\n", what, errno);
        exit(1);
    }
}

static struct ifreq interface(const char *name)
{
    struct ifreq request = {0};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", name);
    return request;
}

static void set_ip(int control, unsigned long operation, const char *value)
{
    struct ifreq request = interface(tun_name);
    struct sockaddr_in addr = {.sin_family = AF_INET};
    check(inet_pton(AF_INET, value, &addr.sin_addr) == 1, "IPv4 address");
    memcpy(&request.ifr_addr, &addr, sizeof(addr));
    check(ioctl(control, operation, &request) == 0, value);
}

static void add_default(unsigned index)
{
    /* Linux NETLINK_ROUTE wire layout: nlmsghdr, rtmsg, RTA_OIF. */
    struct {
        uint32_t len;
        uint16_t type, flags;
        uint32_t seq, pid;
        uint8_t family, dst_len, src_len, tos, table, protocol, scope, kind;
        uint32_t route_flags;
        uint16_t attr_len, attr_type;
        uint32_t ifindex;
    } request = {
        .len = sizeof(request), .type = 24, .flags = 1 | 4 | 0x400 | 0x200,
        .seq = 1, .family = AF_INET, .table = 254, .protocol = 4, .scope = 253,
        .kind = 1, .attr_len = 8, .attr_type = 4, .ifindex = index,
    };
    struct {
        uint16_t family, pad;
        uint32_t pid, groups;
    } address = {.family = AF_NETLINK};
    int fd = socket(AF_NETLINK, SOCK_RAW, 0);
    check(fd >= 0, "route socket");
    check(bind(fd, (void *)&address, sizeof(address)) == 0, "route bind");
    check(send(fd, &request, sizeof(request), 0) == sizeof(request), "route send");
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    check(poll(&ready, 1, 2000) == 1, "route ACK wait");
    unsigned char ack[128];
    check(recv(fd, ack, sizeof(ack), 0) >= 20, "route ACK read");
    int32_t error;
    uint16_t type;
    memcpy(&type, ack + 4, 2);
    memcpy(&error, ack + 16, 4);
    if (error < 0)
        errno = -error;
    check(type == 2 && !error, "add default route via TUN");
    close(fd);
}

int main(void)
{
    int control = socket(AF_INET, SOCK_DGRAM, 0);
    check(control >= 0, "control socket");
    int tun = open("/dev/net/tun", O_RDWR);
    check(tun >= 0, "open TUN");
    struct ifreq request = interface(tun_name);
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    check(ioctl(tun, TUNSETIFF, &request) == 0, "TUNSETIFF");
    set_ip(control, SIOCSIFNETMASK, "255.255.255.255");
    set_ip(control, SIOCSIFADDR, "198.18.0.10");
    request = interface(tun_name);
    request.ifr_mtu = PPP_MTU;
    check(ioctl(control, SIOCSIFMTU, &request) == 0, "TUN MTU");
    request.ifr_flags = IFF_UP;
    check(ioctl(control, SIOCSIFFLAGS, &request) == 0, "TUN up");
    unsigned tun_index = if_nametoindex(tun_name);
    check(tun_index != 0, "TUN index");
    add_default(tun_index);

    unsigned wan = if_nametoindex("en3");
    check(wan != 0, "WAN index");
    request = interface("en3");
    check(ioctl(control, SIOCGIFHWADDR, &request) == 0, "WAN MAC");
    unsigned char mac[6];
    memcpy(mac, request.ifr_hwaddr.sa_data, 6);
    close(control);
    int packet = socket(AF_PACKET, SOCK_RAW, htons(PPP_SESSION));
    check(packet >= 0, "PPPoE packet socket");
    struct sockaddr_ll link = {
        .sll_family = AF_PACKET, .sll_protocol = htons(PPP_SESSION),
        .sll_ifindex = (int)wan, .sll_halen = 6,
    };
    check(bind(packet, (void *)&link, sizeof(link)) == 0, "PPPoE packet bind");
    memcpy(link.sll_addr, peer, 6);

    puts("TUN_ROUTER_READY");
    fflush(stdout);
    struct pollfd ready[] = {
        {.fd = tun, .events = POLLIN}, {.fd = packet, .events = POLLIN},
    };
    unsigned char frame[1514];
    for (;;) {
        check(poll(ready, 2, 10000) > 0, "PPP test packet wait");
        if (ready[0].revents & POLLIN) {
            ssize_t n = read(tun, frame + PPP_OVERHEAD, sizeof(frame) - PPP_OVERHEAD);
            check(n >= 20 && n <= PPP_MTU, "TUN packet length");
            memcpy(frame, peer, 6);
            memcpy(frame + 6, mac, 6);
            const unsigned char header[] = {0x88, 0x64, 0x11, 0, 0x12, 0x34,
                (unsigned char)((n + 2) >> 8), (unsigned char)(n + 2), 0, 0x21};
            memcpy(frame + 12, header, sizeof(header));
            check(sendto(packet, frame, n + PPP_OVERHEAD, 0, (void *)&link,
                         sizeof(link)) == n + PPP_OVERHEAD, "send PPPoE frame");
        }
        if (ready[1].revents & POLLIN) {
            ssize_t n = recv(packet, frame, sizeof(frame), 0);
            if (n < PPP_OVERHEAD || memcmp(frame, mac, 6) || memcmp(frame + 6, peer, 6) ||
                memcmp(frame + 12, "\x88\x64\x11\0\x12\x34", 6) ||
                frame[20] || frame[21] != 0x21)
                continue;
            int len = ((frame[18] << 8) | frame[19]) - 2;
            if (len < 20 || len > PPP_MTU || n < len + PPP_OVERHEAD)
                continue;
            check(write(tun, frame + PPP_OVERHEAD, len) == len, "inject PPP IPv4");
            break;
        }
    }
    close(packet);
    close(tun);
    puts("TUN_ROUTER_CLOSED");
    return 0;
}
