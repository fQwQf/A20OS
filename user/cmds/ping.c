#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

#define DNS_SERVER_IP 0x0302000aU /* 10.0.2.3, QEMU user-net DNS */

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

static uint32_t parse_ipv4(const char *s) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

static void ipv4_to_string(uint32_t addr, char out[16]) {
    snprintf(out, 16, "%u.%u.%u.%u",
             (unsigned)(addr & 0xff),
             (unsigned)((addr >> 8) & 0xff),
             (unsigned)((addr >> 16) & 0xff),
             (unsigned)((addr >> 24) & 0xff));
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
    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t txid = (uint16_t)(0x4200U ^ (uint16_t)getpid());
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)txid;
    query[2] = 0x01; /* recursion desired */
    query[5] = 0x01; /* one question */

    size_t qlen = 12;
    if (dns_encode_name(query, sizeof(query), &qlen, name) < 0 || qlen + 4 > sizeof(query))
        return -1;
    query[qlen++] = 0x00; query[qlen++] = 0x01; /* A */
    query[qlen++] = 0x00; query[qlen++] = 0x01; /* IN */

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof(dns));
    dns.sin_family = AF_INET;
    dns.sin_addr.s_addr = DNS_SERVER_IP;
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
    if (n < 12)
        return -1;
    if (reply[0] != query[0] || reply[1] != query[1])
        return -1;
    if ((reply[3] & 0x0f) != 0)
        return -1;

    uint16_t qd = (uint16_t)((reply[4] << 8) | reply[5]);
    uint16_t an = (uint16_t)((reply[6] << 8) | reply[7]);
    size_t off = 12;
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
    return -1;
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ping HOST|IPv4 [count]\n");
        return 1;
    }

    int count = argc > 2 ? atoi(argv[2]) : 4;
    if (count <= 0)
        count = 4;

    uint32_t addr = parse_ipv4(argv[1]);
    char target[256];
    char addr_text[16];
    strncpy(target, argv[1], sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    if (!addr) {
        if (resolve_a_record(argv[1], &addr) < 0) {
            fprintf(stderr, "ping: cannot resolve %s\n", argv[1]);
            return 1;
        }
        ipv4_to_string(addr, addr_text);
        printf("PING %s (%s)\n", argv[1], addr_text);
    } else {
        ipv4_to_string(addr, addr_text);
        printf("PING %s\n", addr_text);
    }

    int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMP);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr;

    uint16_t id = (uint16_t)getpid();
    int received = 0;
    for (int seq = 1; seq <= count; seq++) {
        uint8_t pkt[64];
        memset(pkt, 0xa5, sizeof(pkt));
        icmp_echo_t *icmp = (icmp_echo_t *)pkt;
        icmp->type = 8;
        icmp->code = 0;
        icmp->id = bswap16(id);
        icmp->seq = bswap16((uint16_t)seq);
        icmp->checksum = 0;
        icmp->checksum = bswap16(checksum(pkt, sizeof(pkt)));

        long start = now_ms();
        if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            perror("sendto");
            continue;
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
                reply->seq == bswap16((uint16_t)seq)) {
                printf("%zd bytes from %s: icmp_seq=%d time=%ld ms\n",
                       n - (ssize_t)ihl, addr_text, seq, now_ms() - start);
                received++;
                ok = 1;
                break;
            }
        }
        if (!ok)
            printf("request timeout for icmp_seq %d\n", seq);
        usleep(100000);
    }

    printf("--- %s ping statistics ---\n", target);
    printf("%d packets transmitted, %d received\n", count, received);
    close(fd);
    return received == count ? 0 : 1;
}
