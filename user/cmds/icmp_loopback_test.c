#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_echo_t;

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        return 0;
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static uint16_t checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

int main(void) {
    int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMP);
    if (fd < 0) {
        perror("socket");
        printf("ICMP_LOOPBACK_TEST: FAIL\n");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint16_t id = (uint16_t)getpid();
    uint8_t pkt[64];
    memset(pkt, 0xa5, sizeof(pkt));
    icmp_echo_t *icmp = (icmp_echo_t *)pkt;
    icmp->type = 8;
    icmp->code = 0;
    icmp->id = bswap16(id);
    icmp->seq = bswap16(1);
    icmp->checksum = 0;
    icmp->checksum = bswap16(checksum(pkt, sizeof(pkt)));

    long start = now_ms();
    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("sendto");
        close(fd);
        printf("ICMP_LOOPBACK_TEST: FAIL\n");
        return 1;
    }

    int ok = 0;
    while (now_ms() - start < 2000) {
        uint8_t buf[1600];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            perror("recvfrom");
            break;
        }
        if (n < 28)
            continue;
        size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
        if (ihl < 20 || (size_t)n < ihl + sizeof(icmp_echo_t))
            continue;
        icmp_echo_t *reply = (icmp_echo_t *)(buf + ihl);
        if (reply->type == 0 && reply->id == bswap16(id) &&
            reply->seq == bswap16(1)) {
            ok = 1;
            break;
        }
    }

    close(fd);
    if (ok) {
        printf("ICMP_LOOPBACK_TEST: PASS\n");
        return 0;
    }
    printf("ICMP_LOOPBACK_TEST: FAIL\n");
    return 1;
}
