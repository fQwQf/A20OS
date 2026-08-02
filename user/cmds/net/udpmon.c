#include <errno.h>
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

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 5555;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "usage: udpmon [port]\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0;
    addr.sin_port = bswap16((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("udpmon: listening on 0.0.0.0:%d\n", port);
    for (;;) {
        char buf[1500];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recvfrom");
            break;
        }
        buf[n] = 0;
        printf("udpmon: %zd bytes: %s\n", n, buf);
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fromlen);
    }
    close(fd);
    return 1;
}
