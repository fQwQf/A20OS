#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
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

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: udpsend IPv4 port message\n");
        return 1;
    }

    uint32_t ip = parse_ipv4(argv[1]);
    int port = atoi(argv[2]);
    if (!ip || port <= 0 || port > 65535) {
        fprintf(stderr, "udpsend: invalid address or port\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip;
    dst.sin_port = bswap16((uint16_t)port);

    size_t len = strlen(argv[3]);
    ssize_t n = sendto(fd, argv[3], len, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) {
        perror("sendto");
        close(fd);
        return 1;
    }
    printf("sent %zd bytes to %s:%d\n", n, argv[1], port);
    close(fd);
    return 0;
}
