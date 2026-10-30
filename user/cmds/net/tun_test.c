/* Minimal L3 TUN gate used by the BSD/F-Stack backend.
 *
 * Creates a TUN interface, configures an IPv4 address, proves that a UDP
 * packet routed to it reaches the character device, then injects an inbound
 * UDP datagram and proves it reaches a socket bound to the TUN address.
 */
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#ifndef IFF_TUN
#define IFF_TUN 0x0001
#endif
#ifndef IFF_NO_PI
#define IFF_NO_PI 0x1000
#endif
#define TUNSETIFF _IOW('T', 202, int)

static int failures;
#define CHECK(cond, what) do { \
    if (!(cond)) { \
        printf("TUN_TEST: FAIL %s (errno=%d)\n", what, errno); \
        failures++; \
    } \
} while (0)

static unsigned short ip_checksum(const unsigned char *b, size_t length)
{
    unsigned sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum += (unsigned)b[i] << (i % 2 ? 0 : 8);
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return (unsigned short)~sum;
}

static int set_addr(int fd, const char *name, unsigned long op, const char *ip)
{
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", name);
    struct sockaddr_in *a = (struct sockaddr_in *)&req.ifr_addr;
    a->sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &a->sin_addr) != 1)
        return -1;
    return ioctl(fd, op, &req);
}

int main(void)
{
    const char *name = "tun0";
    int tun = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (tun < 0) {
        printf("TUN_TEST: SKIP no /dev/net/tun (errno=%d)\n", errno);
        return 77;
    }
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", name);
    req.ifr_flags = IFF_TUN | IFF_NO_PI;
    CHECK(ioctl(tun, TUNSETIFF, &req) == 0, "TUNSETIFF");
    if (failures) return 1;

    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(ctl >= 0, "control socket");
    if (ctl >= 0) {
        CHECK(set_addr(ctl, name, SIOCSIFADDR, "198.18.0.1") == 0,
              "SIOCSIFADDR");
        CHECK(set_addr(ctl, name, SIOCSIFNETMASK, "255.255.255.0") == 0,
              "SIOCSIFNETMASK");
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", name);
        req.ifr_flags = IFF_UP;
        CHECK(ioctl(ctl, SIOCSIFFLAGS, &req) == 0, "SIOCSIFFLAGS up");
    }

    unsigned index = if_nametoindex(name);
    CHECK(index != 0, "if_nametoindex");
    CHECK(if_nametoindex("tun-missing") == 0, "missing TUN name");
    char altname[IF_NAMESIZE];
    CHECK(if_indextoname(index, altname) && !strcmp(altname, name),
          "if_indextoname");
    struct if_nameindex *list = if_nameindex();
    int listed = 0;
    if (list) {
        for (struct if_nameindex *row = list; row->if_index; ++row)
            if (!strcmp(row->if_name, name)) listed = 1;
        if_freenameindex(list);
    }
    CHECK(listed, "if_nameindex lists TUN");

    int udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(udp >= 0, "UDP socket");
    struct sockaddr_in local, peer;
    memset(&local, 0, sizeof(local));
    memset(&peer, 0, sizeof(peer));
    local.sin_family = AF_INET;
    local.sin_port = htons(49153);
    inet_pton(AF_INET, "198.18.0.1", &local.sin_addr);
    peer.sin_family = AF_INET;
    peer.sin_port = htons(49154);
    inet_pton(AF_INET, "198.18.0.2", &peer.sin_addr);

    CHECK(bind(udp, (struct sockaddr *)&local, sizeof(local)) == 0, "bind");
    CHECK(sendto(udp, "tundata", 7, 0, (struct sockaddr *)&peer,
                 sizeof(peer)) == 7, "send via TUN");

    struct pollfd ready = {.fd = tun, .events = POLLIN};
    unsigned char packet[4096];
    int found = 0, n = 0;
    for (int attempt = 0; attempt < 8 && !found; ++attempt) {
        if (poll(&ready, 1, 1000) != 1)
            continue;
        n = (int)read(tun, packet, sizeof(packet));
        size_t ihl = n >= 20 ? (size_t)(packet[0] & 15) * 4 : 0;
        if (n >= (int)ihl + 15 && packet[0] >> 4 == 4 && packet[9] == 17 &&
            !memcmp(packet + ihl + 8, "tundata", 7))
            found = 1;
    }
    CHECK(found, "TUN received routed UDP payload");
    if (found) {
        size_t ihl = (size_t)(packet[0] & 15) * 4;
        CHECK(ip_checksum(packet, ihl) == 0, "TUN IPv4 checksum");
    }

    /* Configuring the TUN address must not turn it into the default route.
     * An off-link destination has to leave through a20n0, so nothing may
     * appear on the TUN queue.  A zero netmask installed as 0.0.0.0/0 is the
     * exact regression this catches. */
    int off = socket(AF_INET, SOCK_DGRAM, 0);
    if (off >= 0) {
        struct sockaddr_in unrouteable;
        memset(&unrouteable, 0, sizeof(unrouteable));
        unrouteable.sin_family = AF_INET;
        unrouteable.sin_port = htons(9);
        inet_pton(AF_INET, "203.0.113.9", &unrouteable.sin_addr);
        (void)sendto(off, "offlink", 7, 0,
                     (struct sockaddr *)&unrouteable, sizeof(unrouteable));
        close(off);
    }
    struct pollfd shadow = {.fd = tun, .events = POLLIN};
    CHECK(poll(&shadow, 1, 200) == 0, "default route is not shadowed by TUN");

    unsigned char input[35];
    memset(input, 0, sizeof(input));
    input[0] = 0x45;
    input[2] = 0;
    input[3] = sizeof(input);
    input[4] = 0x12; input[5] = 0x34;
    input[8] = 64;
    input[9] = 17;
    memcpy(input + 12, &peer.sin_addr, 4);
    memcpy(input + 16, &local.sin_addr, 4);
    memcpy(input + 20, &peer.sin_port, 2);
    memcpy(input + 22, &local.sin_port, 2);
    input[24] = 0; input[25] = 15;
    memcpy(input + 28, "tunback", 7);
    unsigned short check = htons(ip_checksum(input, 20));
    memcpy(input + 10, &check, 2);
    CHECK(write(tun, input, sizeof(input)) == (ssize_t)sizeof(input),
          "TUN injection write");

    ready = (struct pollfd){.fd = udp, .events = POLLIN};
    int received = poll(&ready, 1, 3000) == 1 &&
                   recv(udp, packet, sizeof(packet), 0) == 7 &&
                   !memcmp(packet, "tunback", 7);
    CHECK(received, "injected TUN packet reaches bound UDP socket");

    close(udp);
    if (ctl >= 0) close(ctl);
    close(tun);
    printf("TUN_TEST: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
