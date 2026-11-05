/* Keep a connected socket open while the serial test changes its policy. */
#include <arpa/inet.h>
#include <err.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static void endpoint(struct sockaddr_storage *address, int family,
                     const char *ip, unsigned port)
{
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)address;
        a->sin_family = family;
        a->sin_port = htons(port);
        if (inet_pton(family, ip, &a->sin_addr) != 1)
            errx(1, "invalid IPv4 address %s", ip);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)address;
        a->sin6_family = family;
        a->sin6_port = htons(port);
        if (inet_pton(family, ip, &a->sin6_addr) != 1)
            errx(1, "invalid IPv6 address %s", ip);
    }
}

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 7)
        errx(1, "usage: policy-local-probe udp|tcp source|- destination port command-file [device]");
    bool tcp = strcmp(argv[1], "tcp") == 0;
    int family = strchr(argv[3], ':') ? AF_INET6 : AF_INET;
    socklen_t addrlen = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    int fd = socket(family, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0)
        err(1, "socket");
    if (argc == 7 && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, argv[6], strlen(argv[6]) + 1) < 0)
        err(1, "SO_BINDTODEVICE");
    if (strcmp(argv[2], "-")) {
        struct sockaddr_storage local = {0};
        endpoint(&local, family, argv[2], 0);
        if (bind(fd, (struct sockaddr *)&local, addrlen) < 0)
            err(1, "bind");
    }
    struct sockaddr_storage remote = {0};
    endpoint(&remote, family, argv[3], strtoul(argv[4], NULL, 10));
    struct timeval timeout = {.tv_sec = 8};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
        err(1, "setsockopt");
    if (connect(fd, (struct sockaddr *)&remote, addrlen) < 0)
        err(1, "connect");
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\nA20_POLICY_PROBE_READY %s\n", argv[5]);
    char previous[64] = {0};
    for (;;) {
        usleep(20000);
        int command = open(argv[5], O_RDONLY);
        if (command < 0)
            continue;
        char tag[64];
        ssize_t length = read(command, tag, sizeof(tag) - 1);
        close(command);
        if (length < 2 || tag[length - 1] != '\n')
            continue;
        tag[--length] = '\0';
        if (strcmp(tag, previous) == 0)
            continue;
        if (strcmp(tag, "quit") == 0)
            break;
        memcpy(previous, tag, length + 1);
        if (send(fd, tag, length, 0) != length)
            err(1, "send");
        char reply[64];
        ssize_t received = recv(fd, reply, length, tcp ? MSG_WAITALL : 0);
        if (received != length || memcmp(reply, tag, length))
            errx(1, "unexpected reply for %s (%zd bytes)", tag, received);
        printf("\nA20_POLICY_PROBE_ECHO %s\n", tag);
    }
    return close(fd) < 0;
}
