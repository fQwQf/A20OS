/* Verify that link events and SIOCGIFFLAGS expose the same carrier changes. */
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool running(int ctl, const char *name)
{
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    if (ioctl(ctl, SIOCGIFFLAGS, &ifr) < 0)
        err(1, "SIOCGIFFLAGS");
    if (!(ifr.ifr_flags & IFF_UP))
        errx(1, "carrier change altered admin state");
    return (ifr.ifr_flags & IFF_RUNNING) != 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        errx(1, "usage: carrier-watch INTERFACE");
    unsigned index = if_nametoindex(argv[1]);
    if (!index)
        err(1, "if_nametoindex");
    int ctl = socket(AF_INET, SOCK_DGRAM, 0);
    int nl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
    struct sockaddr_nl subscription = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};
    if (ctl < 0 || nl < 0 || bind(nl, (struct sockaddr *)&subscription, sizeof(subscription)) < 0)
        err(1, "subscribe to link events");
    if (!running(ctl, argv[1]))
        errx(1, "initial carrier is down");
    puts("A20_CARRIER_READY");
    fflush(stdout);

    for (unsigned expected = 0; expected < 2; ++expected) {
        for (;;) {
            struct pollfd wait = {.fd = nl, .events = POLLIN};
            if (poll(&wait, 1, 5000) != 1)
                errx(1, "no carrier event");
            union { struct nlmsghdr aligned; char bytes[2048]; } buffer;
            int length = recv(nl, buffer.bytes, sizeof(buffer.bytes), 0);
            if (length < 0)
                err(1, "receive link event");
            struct nlmsghdr *msg = &buffer.aligned;
            if (!NLMSG_OK(msg, length) || msg->nlmsg_type != RTM_NEWLINK ||
                NLMSG_PAYLOAD(msg, 0) < sizeof(struct ifinfomsg))
                continue;
            const struct ifinfomsg *link = NLMSG_DATA(msg);
            if ((unsigned)link->ifi_index != index)
                continue;
            if (!(link->ifi_flags & IFF_UP) || !!(link->ifi_flags & IFF_RUNNING) != expected ||
                running(ctl, argv[1]) != (bool)expected)
                errx(1, "inconsistent carrier transition");
            puts(expected ? "A20_CARRIER_UP" : "A20_CARRIER_DOWN");
            fflush(stdout);
            break;
        }
    }

    if (setsockopt(ctl, SOL_SOCKET, SO_BINDTODEVICE, argv[1], strlen(argv[1]) + 1) < 0)
        err(1, "bind UDP to device");
    struct sockaddr_in wrong = {.sin_family = AF_INET, .sin_port = htons(42000)};
    inet_pton(AF_INET, "192.168.10.2", &wrong.sin_addr);
    if (sendto(ctl, "wrong", 5, 0, (struct sockaddr *)&wrong, sizeof(wrong)) != -1 ||
        errno != EHOSTUNREACH)
        errx(1, "bound IPv4 socket permitted another egress interface");
    puts("A20_BOUND_WRONG_EGRESS_REJECTED");
    close(nl);
    close(ctl);
    return 0;
}
