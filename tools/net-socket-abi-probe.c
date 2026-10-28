/* A single Linux ELF probes service-facing socket semantics on both kernels.
 * Usage: net-socket-abi-probe INTERFACE LOCAL_IPV4. No A20OS headers. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void require(int ok, const char *what)
{
    if (!ok) { perror(what); exit(1); }
}

static struct sockaddr_in address(const char *ip, unsigned port)
{
    struct sockaddr_in out = { .sin_family = AF_INET, .sin_port = htons(port) };
    require(inet_pton(AF_INET, ip, &out.sin_addr) == 1, "inet_pton");
    return out;
}

static int endpoint(int type, const char *ip, unsigned port)
{
    int fd = socket(AF_INET, type, 0);
    require(fd >= 0, "socket");
    struct sockaddr_in local = address(ip, port);
    require(bind(fd, (void *)&local, sizeof(local)) == 0, "bind concrete address");
    return fd;
}

static void receive_packet(int fd, size_t expected, struct in_addr local)
{
    struct pollfd watch = { .fd = fd, .events = POLLIN };
    require(poll(&watch, 1, 2000) == 1 && (watch.revents & POLLIN), "UDP readiness");
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control;
    char payload[2];
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    ssize_t got = recvmsg(fd, &message, 0);
    require(got == (ssize_t)expected, "UDP payload length including zero");
    int found = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != IPPROTO_IP || c->cmsg_type != IP_PKTINFO) continue;
        require(c->cmsg_len == CMSG_LEN(sizeof(struct in_pktinfo)), "IP_PKTINFO length");
        struct in_pktinfo info;
        memcpy(&info, CMSG_DATA(c), sizeof(info));
        require(info.ipi_ifindex > 0 && info.ipi_addr.s_addr == local.s_addr &&
                info.ipi_spec_dst.s_addr == local.s_addr, "IP_PKTINFO values");
        found = 1;
    }
    require(found && !(message.msg_flags & MSG_CTRUNC), "IP_PKTINFO present");
}

int main(int argc, char **argv)
{
    require(argc == 3, "usage: net-socket-abi-probe INTERFACE LOCAL_IPV4");
    unsigned index = if_nametoindex(argv[1]);
    require(index != 0, "if_nametoindex");
    int tcp_a = endpoint(SOCK_STREAM, "127.0.0.1", 40253);
    int tcp_b = endpoint(SOCK_STREAM, argv[2], 40253);
    require(listen(tcp_a, 2) == 0 && listen(tcp_b, 2) == 0, "distinct listeners");
    int wildcard = socket(AF_INET, SOCK_STREAM, 0);
    require(wildcard >= 0, "wildcard socket");
    struct sockaddr_in any = address("0.0.0.0", 40253);
    require(bind(wildcard, (void *)&any, sizeof(any)) < 0 && errno == EADDRINUSE,
            "wildcard must conflict with concrete listeners");
    close(wildcard);
    close(tcp_b);
    close(tcp_a);

    int receiver = endpoint(SOCK_DGRAM, argv[2], 40254);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    require(sender >= 0, "sender");
    struct ifreq request;
    memset(&request, 0xa5, sizeof(request));
    request.ifr_ifindex = (int)index;
    require(ioctl(sender, SIOCGIFNAME, &request) == 0 &&
            strcmp(request.ifr_name, argv[1]) == 0, "SIOCGIFNAME ignores input name");
    require(setsockopt(sender, SOL_SOCKET, SO_BINDTODEVICE, "missing-xyz", 12) < 0 &&
            errno == ENODEV, "unknown interface must fail");
    require(setsockopt(sender, SOL_SOCKET, SO_BINDTODEVICE, argv[1], strlen(argv[1])+1) == 0,
            "SO_BINDTODEVICE");
    char name[IFNAMSIZ] = {0};
    socklen_t length = sizeof(name);
    require(getsockopt(sender, SOL_SOCKET, SO_BINDTODEVICE, name, &length) == 0 &&
            strcmp(name, argv[1]) == 0, "bound device readback");
    int one = 1, mode = IP_PMTUDISC_DONT;
    require(setsockopt(receiver, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) == 0,
            "enable IP_PKTINFO");
    require(setsockopt(sender, IPPROTO_IP, IP_MTU_DISCOVER, &mode, sizeof(mode)) == 0,
            "IP_PMTUDISC_DONT");
    struct sockaddr_in destination = address(argv[2], 40254);
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control = {0};
    struct msghdr message = { .msg_name = &destination, .msg_namelen = sizeof(destination),
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = IP_PKTINFO;
    c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo info = { .ipi_ifindex = (int)index, .ipi_spec_dst = destination.sin_addr };
    memcpy(CMSG_DATA(c), &info, sizeof(info));
    /* Empty iovec still emits a UDP packet and honors its ancillary data. */
    require(sendmsg(sender, &message, 0) == 0, "zero-length controlled UDP");
    receive_packet(receiver, 0, destination.sin_addr);
    require(connect(sender, (void *)&destination, sizeof(destination)) == 0, "UDP connect");
    char byte = 'a';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    message.msg_name = NULL;
    message.msg_namelen = 0;
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    require(sendmsg(sender, &message, 0) == 1, "connected controlled UDP");
    receive_packet(receiver, 1, destination.sin_addr);
    require(setsockopt(sender, SOL_SOCKET, SO_BINDTODEVICE, "", 0) == 0, "clear device binding");
    length = sizeof(name);
    require(getsockopt(sender, SOL_SOCKET, SO_BINDTODEVICE, name, &length) == 0 && length == 0,
            "cleared binding length");
    close(sender);
    close(receiver);
    puts("NET_SOCKET_ABI_PASS");
    return 0;
}
