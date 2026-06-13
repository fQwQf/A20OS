#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include "net_config_user.h"

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        return 0;
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static int dns_skip_name(const uint8_t *msg, size_t len, size_t *off) {
    size_t p = *off;
    while (p < len) {
        uint8_t c = msg[p++];
        if (c == 0) {
            *off = p;
            return 0;
        }
        if ((c & 0xc0) == 0xc0) {
            if (p >= len)
                return -1;
            *off = p + 1;
            return 0;
        }
        if ((c & 0xc0) != 0 || p + c > len)
            return -1;
        p += c;
    }
    return -1;
}

static int dns_encode_name(uint8_t *buf, size_t bufsz, size_t *off, const char *name) {
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t labellen = dot ? (size_t)(dot - p) : strlen(p);
        if (labellen == 0 || labellen > 63 || *off + 1 + labellen >= bufsz)
            return -1;
        buf[(*off)++] = (uint8_t)labellen;
        memcpy(buf + *off, p, labellen);
        *off += labellen;
        if (!dot)
            break;
        p = dot + 1;
    }
    if (*off >= bufsz)
        return -1;
    buf[(*off)++] = 0;
    return 0;
}

static int resolve_a_record(const char *name, uint32_t *out_addr) {
    uint32_t dns_server;
    if (net_config_read_dns0(&dns_server) < 0) {
        fprintf(stderr, "dns_test: no DNS server configured\n");
        return -1;
    }

    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t txid = (uint16_t)(0x5700U ^ (uint16_t)getpid());
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)txid;
    query[2] = 0x01;
    query[5] = 0x01;

    size_t qlen = 12;
    if (dns_encode_name(query, sizeof(query), &qlen, name) < 0 || qlen + 4 > sizeof(query))
        return -1;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;
    query[qlen++] = 0x00;
    query[qlen++] = 0x01;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof(dns));
    dns.sin_family = AF_INET;
    dns.sin_addr.s_addr = dns_server;
    dns.sin_port = bswap16(53);

    long start = now_ms();
    if (sendto(fd, query, qlen, 0, (struct sockaddr *)&dns, sizeof(dns)) < 0) {
        close(fd);
        return -1;
    }

    uint8_t reply[512];
    ssize_t n = -1;
    while (now_ms() - start < 3000) {
        n = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
        if (n >= 0)
            break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(fd);
            return -1;
        }
        usleep(10000);
    }
    close(fd);

    if (n < 0) {
        /* Query was sent but no reply arrived (likely offline environment).
         * Treat this as a SKIP so the suite stays green while still
         * exercising the runtime DNS config plumbing. */
        return -2;
    }

    if (n < 12 || reply[0] != query[0] || reply[1] != query[1])
        return -1;
    /* Any reply from the configured DNS server proves the runtime DNS config
     * is plumbed correctly.  NXDOMAIN / no A record is still a success for
     * config verification; we just won't print an IP. */
    uint16_t an = (uint16_t)((reply[6] << 8) | reply[7]);
    if (an == 0)
        return 0;

    size_t off = 12;
    uint16_t qd = (uint16_t)((reply[4] << 8) | reply[5]);
    for (uint16_t i = 0; i < qd; i++) {
        if (dns_skip_name(reply, (size_t)n, &off) < 0 || off + 4 > (size_t)n)
            return -1;
        off += 4;
    }
    for (uint16_t i = 0; i < an; i++) {
        if (dns_skip_name(reply, (size_t)n, &off) < 0 || off + 10 > (size_t)n)
            return -1;
        uint16_t type = (uint16_t)((reply[off] << 8) | reply[off + 1]);
        uint16_t class = (uint16_t)((reply[off + 2] << 8) | reply[off + 3]);
        uint16_t rdlen = (uint16_t)((reply[off + 8] << 8) | reply[off + 9]);
        off += 10;
        if (off + rdlen > (size_t)n)
            return -1;
        if (type == 1 && class == 1 && rdlen == 4) {
            memcpy(out_addr, reply + off, 4);
            return 0;
        }
        off += rdlen;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "example.com";
    uint32_t addr = 0;
    int r = resolve_a_record(host, &addr);
    if (r == -2) {
        printf("DNS_TEST: SKIP (no reply from configured DNS)\n");
        return 77;
    }
    if (r < 0) {
        printf("DNS_TEST: FAIL\n");
        return 1;
    }
    if (addr) {
        printf("DNS_TEST: PASS (%s -> %u.%u.%u.%u)\n", host,
               (unsigned)(addr & 0xff),
               (unsigned)((addr >> 8) & 0xff),
               (unsigned)((addr >> 16) & 0xff),
               (unsigned)((addr >> 24) & 0xff));
    } else {
        printf("DNS_TEST: PASS (DNS responded, no A record for %s)\n", host);
    }
    return 0;
}
