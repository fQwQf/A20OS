#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static long now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        return 0;
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static int recv_timeout_test(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;

    struct timeval tv = { 0, 200000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(12347);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char buf[8];
    long start = now_ms();
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    long elapsed = now_ms() - start;
    close(fd);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) &&
        elapsed >= 150 && elapsed <= 500)
        return 0;
    return -1;
}

static int connect_timeout_test(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct timeval tv = { 0, 200000 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl((10U << 24) | (2 << 16) | 99);
    addr.sin_port = htons(12348);

    long start = now_ms();
    int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    long elapsed = now_ms() - start;
    close(fd);

    if (r < 0 && (errno == ETIMEDOUT || errno == EAGAIN || errno == EWOULDBLOCK) &&
        elapsed >= 150 && elapsed <= 500)
        return 0;
    return -1;
}

int main(void) {
    int r1 = recv_timeout_test();
    int r2 = connect_timeout_test();

    if (r1 == 0 && r2 == 0) {
        printf("TIMEOUT_TEST: PASS\n");
        return 0;
    }
    printf("TIMEOUT_TEST: FAIL (recv=%d connect=%d)\n", r1, r2);
    return 1;
}
