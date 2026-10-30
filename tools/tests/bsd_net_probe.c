#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    static const unsigned char query[] = {
        0x12,0x34, 0x01,0x00, 0x00,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00,
        7,'e','x','a','m','p','l','e',3,'c','o','m',0, 0x00,0x01, 0x00,0x01
    };
    struct sockaddr_in dns = {.sin_family = AF_INET, .sin_port = htons(53)};
    inet_pton(AF_INET, "10.0.2.3", &dns.sin_addr);
    errno = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    printf("PROBE_SOCKET fd=%d errno=%d\n", fd, errno);
    if (fd < 0) return 1;
    struct sockaddr_in local = {0};
    socklen_t local_len = sizeof(local);
    errno = 0;
    int rc = getsockname(fd, (void *)&local, &local_len);
    printf("PROBE_GETSOCKNAME rc=%d errno=%d family=%d port=%u addr=%08x\n",
           rc, errno, local.sin_family, ntohs(local.sin_port), local.sin_addr.s_addr);
    errno = 0;
    ssize_t sent = sendto(fd, query, sizeof(query), 0, (void *)&dns, sizeof(dns));
    printf("PROBE_SENDTO rc=%ld errno=%d\n", (long)sent, errno);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    errno = 0;
    rc = poll(&pfd, 1, 5000);
    printf("PROBE_POLL rc=%d revents=%x errno=%d\n", rc, pfd.revents, errno);
    unsigned char answer[512];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);
    errno = 0;
    ssize_t got = recvfrom(fd, answer, sizeof(answer), 0, (void *)&from, &from_len);
    printf("PROBE_RECVFROM rc=%ld errno=%d family=%d port=%u addr=%08x len=%u\n",
           (long)got, errno, from.sin_family, ntohs(from.sin_port),
           from.sin_addr.s_addr, from_len);
    close(fd);
    return got > 0 ? 0 : 1;
}
