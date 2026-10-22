/*
 * External UDP data-path test: 16 patterned datagrams arrive from the host
 * through QEMU's user-net, each is verified and echoed back with a one-byte
 * transform.  The host probe checks the returned bytes, so both directions and
 * the IP/UDP checksums are exercised on a real virtio-net device rather than
 * the loopback fast path.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define TEST_PORT 18085
#define TRANSACTIONS 16
#define PAYLOAD_LEN 512

static unsigned char pattern(size_t pos, uint32_t seq) {
    return (unsigned char)((pos * 17u + seq * 29u + 0x3du) & 0xffu);
}

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("UDP_HOSTFWD_TEST: FAIL socket (errno=%d)\n", errno);
        return 1;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(TEST_PORT);
    a.sin_addr.s_addr = inet_addr("10.0.2.15");
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("UDP_HOSTFWD_TEST: FAIL bind (errno=%d %s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned completed = 0;
    while (completed < TRANSACTIONS) {
        unsigned char buf[PAYLOAD_LEN + 8];
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 8) {
            printf("UDP_HOSTFWD_TEST: FAIL recv rc=%zd errno=%d %s\n",
                   n, errno, strerror(errno));
            close(fd);
            return 1;
        }
        uint32_t seq;
        memcpy(&seq, buf, 4);
        uint16_t len;
        memcpy(&len, buf + 4, 2);
        if (n != (ssize_t)(6 + len)) {
            printf("UDP_HOSTFWD_TEST: FAIL malformed len=%u rc=%zd\n", len, n);
            close(fd);
            return 1;
        }
        for (size_t i = 0; i < len; i++) {
            if (buf[6 + i] != pattern(i, seq)) {
                printf("UDP_HOSTFWD_TEST: FAIL payload seq=%u pos=%zu\n", seq,
                       i);
                close(fd);
                return 1;
            }
        }
        /* One-byte transform on the way back, so an echo that never left the
         * host side cannot satisfy the probe. */
        for (size_t i = 0; i < len; i++)
            buf[6 + i] = (unsigned char)(buf[6 + i] ^ 0xA5u);
        if (sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&peer, plen) != n) {
            printf("UDP_HOSTFWD_TEST: FAIL echo rc errno=%d %s\n", errno,
                   strerror(errno));
            close(fd);
            return 1;
        }
        completed++;
    }
    close(fd);
    printf("UDP_HOSTFWD_TEST: PASS port=%d transactions=%u bytes=%u\n",
           TEST_PORT, completed, completed * PAYLOAD_LEN);
    return 0;
}
