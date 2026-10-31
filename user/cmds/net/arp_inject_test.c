/*
 * SIOCSARP gate.
 *
 * A DHCP server answers a DISCOVER by unicasting to an address the client has
 * not configured yet, so it first injects the client's MAC with SIOCSARP.
 * Before this existed the OFFER was queued behind an ARP that could never
 * resolve and the lease never completed.
 *
 * This checks the ioctl contract only: a complete Ethernet entry on a real
 * interface is accepted, the same call again refreshes it, and an entry that
 * names an interface which does not exist is rejected.
 */
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SIOCSARP
#define SIOCSARP 0x8955
#endif
#ifndef ATF_COM
#define ATF_COM 0x02
#endif

#define TEST_NAME "ARP_INJECT"

struct test_arpreq {
    struct sockaddr arp_pa;
    struct sockaddr arp_ha;
    int arp_flags;
    struct sockaddr arp_netmask;
    char arp_dev[16];
};

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("%s: ok   %s\n", TEST_NAME, what);
    } else {
        failures++;
        printf("%s: FAIL %s\n", TEST_NAME, what);
    }
}

static int arp_set(int fd, const char *device, unsigned address, int flags)
{
    struct test_arpreq req;
    memset(&req, 0, sizeof(req));
    req.arp_pa.sa_family = AF_INET;
    memcpy(req.arp_pa.sa_data + 2, &address, 4);
    req.arp_ha.sa_family = 1 /* ARPHRD_ETHER */;
    req.arp_ha.sa_data[0] = 0x52;
    req.arp_ha.sa_data[1] = 0x54;
    req.arp_ha.sa_data[2] = 0x00;
    req.arp_ha.sa_data[3] = 0xaa;
    req.arp_ha.sa_data[4] = 0xbb;
    req.arp_ha.sa_data[5] = 0xcc;
    req.arp_flags = flags;
    if (device) snprintf(req.arp_dev, sizeof(req.arp_dev), "%s", device);
    return ioctl(fd, SIOCSARP, &req);
}

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("%s: FAIL socket errno=%d\n", TEST_NAME, errno);
        return 1;
    }

    if (if_nametoindex("en2") == 0) {
        printf("%s: SKIP no en2\n", TEST_NAME);
        close(fd);
        return 0;
    }

    unsigned address = 0;
    {
        /* The entry must fall inside an installed route (in_lltable_rtcheck),
         * so derive it from en2's own address and mask. */
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "en2");
        if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
            printf("%s: SKIP no en2 address\n", TEST_NAME);
            close(fd);
            return 0;
        }
        uint32_t local, mask;
        memcpy(&local, ifr.ifr_addr.sa_data + 2, 4);
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "en2");
        if (ioctl(fd, SIOCGIFNETMASK, &ifr) < 0) {
            printf("%s: SKIP no en2 netmask\n", TEST_NAME);
            close(fd);
            return 0;
        }
        memcpy(&mask, ifr.ifr_addr.sa_data + 2, 4);
        /* local | ~mask, stepped back two hosts: always on-link and not the
         * broadcast address.  Work on the wire bytes so the result does not
         * depend on host endianness. */
        uint8_t lb[4], mb[4], ab[4];
        memcpy(lb, &local, 4);
        memcpy(mb, &mask, 4);
        for (int i = 0; i < 4; i++) ab[i] = lb[i] | (uint8_t)~mb[i];
        ab[3] = (uint8_t)(ab[3] - 2);
        memcpy(&address, ab, 4);
    }
    errno = 0;
    int rc = arp_set(fd, "en2", address, ATF_COM);
    if (rc != 0) printf("%s: note add errno=%d\n", TEST_NAME, errno);
    ok(rc == 0, "SIOCSARP adds a complete neighbour entry");
    errno = 0;
    rc = arp_set(fd, "en2", address, ATF_COM);
    if (rc != 0) printf("%s: note refresh errno=%d\n", TEST_NAME, errno);
    ok(rc == 0, "SIOCSARP refreshes an existing entry");

    errno = 0;
    ok(arp_set(fd, "nosuch0", address, ATF_COM) < 0 && errno == ENODEV,
       "SIOCSARP rejects an unknown device");

    errno = 0;
    ok(arp_set(fd, "en2", address, 0) < 0 && errno == EOPNOTSUPP,
       "SIOCSARP rejects an incomplete entry");

    /*
     * Now do what a DHCP server does next: send a unicast datagram to the
     * injected address with IP_PKTINFO naming the interface, exactly the way
     * dnsmasq sends an OFFER.  The plain sends first prove that the socket
     * send path itself still works after the injection.
     */
    {
        int udp = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        if (udp < 0) {
            printf("%s: note udp socket errno=%d\n", TEST_NAME, errno);
        } else {
            (void)setsockopt(udp, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
            {
                struct sockaddr_in local;
                memset(&local, 0, sizeof(local));
                local.sin_family = AF_INET;
                local.sin_port = htons(68);
                (void)bind(udp, (struct sockaddr *)&local, sizeof(local));
            }
            char payload[8] = "offer";
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_port = htons(68);
            dst.sin_addr.s_addr = htonl(0x0a000202); /* 10.0.2.2 */
            memcpy(&dst.sin_addr, &address, 4);
            ssize_t n = sendto(udp, payload, sizeof(payload), 0,
                               (struct sockaddr *)&dst, sizeof(dst));
            ok(n == (ssize_t)sizeof(payload),
               "plain unicast to an injected neighbour is accepted");

            struct {
                struct cmsghdr hdr;
                struct { int ifindex; struct in_addr spec; struct in_addr addr; } info;
            } control;
            memset(&control, 0, sizeof(control));
            control.info.ifindex = (int)if_nametoindex("en2");
            struct iovec iov = { payload, sizeof(payload) };
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_name = &dst;
            msg.msg_namelen = sizeof(dst);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = &control;
            msg.msg_controllen = sizeof(control);
            control.hdr.cmsg_len = sizeof(control);
            control.hdr.cmsg_level = IPPROTO_IP;
            control.hdr.cmsg_type = 8 /* IP_PKTINFO */;

            memcpy(&dst.sin_addr, &address, 4);
            ssize_t sent = sendmsg(udp, &msg, 0);
            ok(sent == (ssize_t)sizeof(payload),
               "IP_PKTINFO unicast to an injected neighbour is accepted");
            close(udp);
        }
    }
    close(fd);
    if (failures) {
        printf("%s: FAIL (%d failures, %d checks)\n", TEST_NAME, failures, checks);
        return 1;
    }
    printf("%s: PASS (%d checks)\n", TEST_NAME, checks);
    return 0;
}
