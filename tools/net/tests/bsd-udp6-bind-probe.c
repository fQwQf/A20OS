#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void require(int condition, const char *what)
{
    if (!condition) {
        perror(what);
        exit(1);
    }
}

static void receive(int fd, const char *expected, unsigned ifindex)
{
    struct pollfd waiting = {.fd = fd, .events = POLLIN};
    require(poll(&waiting, 1, 5000) == 1, "poll");
    struct sockaddr_in6 source;
    char payload[32];
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(struct in6_pktinfo))];
    } control;
    struct iovec buffer = {.iov_base = payload, .iov_len = sizeof(payload)};
    struct msghdr msg = {
        .msg_name = &source, .msg_namelen = sizeof(source),
        .msg_iov = &buffer, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
    };
    ssize_t received = recvmsg(fd, &msg, 0);
    require(received == (ssize_t)strlen(expected) && !memcmp(payload, expected, received),
            "wrong datagram");
    require(source.sin6_scope_id == ifindex, "wrong source scope");
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    require(cmsg && cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(struct in6_pktinfo)), "missing IPv6 pktinfo");
    struct in6_pktinfo *info = (void *)CMSG_DATA(cmsg);
    require(info->ipi6_ifindex == ifindex, "wrong pktinfo interface");
    require(sendto(fd, payload, received, 0, (void *)&source, sizeof(source)) == received,
            "UDP reply");
}

int main(void)
{
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    require(fd >= 0, "socket");
    require(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "en3", 4) == 0, "bind en3");
    char device[16] = {0};
    socklen_t size = sizeof(device);
    require(getsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device, &size) == 0 &&
            !strcmp(device, "en3"), "get bound device");
    int one = 1;
    require(setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) == 0,
            "enable IPv6 pktinfo");
    struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_port = htons(45678)};
    require(bind(fd, (void *)&local, sizeof(local)) == 0, "bind UDP");
    puts("A20_UDP6_BOUND");
    fflush(stdout);
    receive(fd, "wan", 3);

    struct sockaddr_in6 wrong = {
        .sin6_family = AF_INET6, .sin6_port = htons(45679), .sin6_scope_id = 1,
    };
    require(inet_pton(AF_INET6, "fe80::2", &wrong.sin6_addr) == 1, "inet_pton");
    require(sendto(fd, "wrong", 5, 0, (void *)&wrong, sizeof(wrong)) == -1,
            "bound socket sent through LAN");
    require(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "", 1) == 0, "unbind en3");
    puts("A20_UDP6_UNBOUND");
    fflush(stdout);
    receive(fd, "lan", 1);
    close(fd);
    puts("A20_UDP6_BIND_PASS");
    return 0;
}
