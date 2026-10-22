/*
 * Long-run IPv6 receive soak.
 *
 * NET-09 is about pool corruption on the IPv6 receive path: the historical
 * reproducer was a desktop that panicked after ~1 min of IPv6 traffic with
 * `pbuf_free: p->ref > 0` or, with the pool canary enabled, an underflow in
 * PBUF_POOL.  This test drives the same alloc/free surface deterministically:
 * UDP6 datagrams sent to ::1 traverse ip6_input, pbuf allocation and the
 * socket receive path, and the sizes cross the single-element boundary so both
 * the one-pbuf and chained-pbuf shapes are exercised.
 *
 * It is not a device-RX test.  The historical failure needed a live virtio
 * netif, which QEMU user-net cannot drive for IPv6 without an external peer;
 * this covers the protocol/pbuf half and is run with
 * CONFIG_LWIP_MEMP_OVERFLOW_CHECK=1 so an out-of-bounds write into the pool
 * stops the machine at the first allocation rather than corrupting later state.
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

#define SOAK_ROUNDS 4000
#define SOAK_MAX_PAYLOAD 16384

static const size_t sizes[] = { 64, 512, 1460, 4000, 8000, 16000 };
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))

int main(void) {
    int rx = socket(AF_INET6, SOCK_DGRAM, 0);
    int tx = socket(AF_INET6, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0) {
        printf("IPV6_RX_SOAK: FAIL socket (errno=%d %s)\n", errno, strerror(errno));
        return 1;
    }
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(13004);
    inet_pton(AF_INET6, "::1", &a.sin6_addr);
    if (bind(rx, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("IPV6_RX_SOAK: FAIL bind (errno=%d %s)\n", errno, strerror(errno));
        close(rx);
        close(tx);
        return 1;
    }
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static unsigned char txbuf[SOAK_MAX_PAYLOAD];
    static unsigned char rxbuf[SOAK_MAX_PAYLOAD];
    for (size_t i = 0; i < SOAK_MAX_PAYLOAD; i++)
        txbuf[i] = (unsigned char)(i * 31u + 7u);

    unsigned long long bytes = 0;
    for (int round = 0; round < SOAK_ROUNDS; round++) {
        size_t len = sizes[round % NSIZES];
        ssize_t w = sendto(tx, txbuf, len, 0, (struct sockaddr *)&a, sizeof(a));
        if (w != (ssize_t)len) {
            printf("IPV6_RX_SOAK: FAIL send round=%d len=%zu rc=%zd errno=%d %s\n",
                   round, len, w, errno, strerror(errno));
            close(rx);
            close(tx);
            return 1;
        }
        ssize_t r = recv(rx, rxbuf, sizeof(rxbuf), 0);
        if (r != (ssize_t)len || memcmp(txbuf, rxbuf, len) != 0) {
            printf("IPV6_RX_SOAK: FAIL recv round=%d len=%zu rc=%zd errno=%d %s\n",
                   round, len, r, errno, strerror(errno));
            close(rx);
            close(tx);
            return 1;
        }
        bytes += (unsigned long long)len * 2;
    }
    close(rx);
    close(tx);
    printf("IPV6_RX_SOAK: PASS rounds=%d bytes=%llu\n", SOAK_ROUNDS, bytes);
    return 0;
}
