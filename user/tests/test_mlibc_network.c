#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "MLIBC_NETWORK: FAIL line=%d errno=%d\n",          \
                    __LINE__, errno);                                          \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static unsigned char payload[8192], received[16384];
static void ready(int fd, short events) {
    struct pollfd item = {fd, events, 0};
    int result = poll(&item, 1, 5000);
    if (result != 1 || !(item.revents & events))
        fprintf(stderr,
                "MLIBC_NETWORK: poll fd=%d events=%x result=%d revents=%x\n",
                fd, events, result, item.revents);
    CHECK(result == 1 && (item.revents & events));
}
static void no_datagram(int fd) {
    char value;
    errno = 0;
    CHECK(recv(fd, &value, 1, MSG_DONTWAIT) == -1 &&
          (errno == EAGAIN || errno == EWOULDBLOCK));
}
static void datagrams_v4(void) {
    int sender = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(sender >= 0 && receiver >= 0);
    struct sockaddr_in local = {.sin_family = AF_INET,
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(sender, (void *)&local, sizeof(local)) == 0);
    CHECK(bind(receiver, (void *)&local, sizeof(local)) == 0);
    struct sockaddr_in destination, source_expected;
    socklen_t size = sizeof(destination);
    CHECK(getsockname(receiver, (void *)&destination, &size) == 0 &&
          size == sizeof(destination) && destination.sin_port);
    size = sizeof(source_expected);
    CHECK(getsockname(sender, (void *)&source_expected, &size) == 0 &&
          source_expected.sin_port);
    struct iovec parts[32];
    for (unsigned i = 0; i < 32; i++)
        parts[i] = (struct iovec){payload + i * 256, 256};
    struct msghdr message = {.msg_name = &destination,
                             .msg_namelen = sizeof(destination),
                             .msg_iov = parts,
                             .msg_iovlen = 32};
    CHECK(sendmsg(sender, &message, 0) == sizeof(payload));
    ready(receiver, POLLIN);
    struct sockaddr_storage source;
    for (unsigned i = 0; i < 32; i++)
        parts[i] = (struct iovec){received + i * 256, 256};
    message = (struct msghdr){.msg_name = &source,
                              .msg_namelen = sizeof(source),
                              .msg_iov = parts,
                              .msg_iovlen = 32};
    CHECK(recvmsg(receiver, &message, 0) == sizeof(payload));
    CHECK(message.msg_namelen == sizeof(struct sockaddr_in) &&
          !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)));
    CHECK(!memcmp(payload, received, sizeof(payload)));
    struct sockaddr_in *from = (void *)&source;
    CHECK(from->sin_family == AF_INET &&
          from->sin_port == source_expected.sin_port &&
          from->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    no_datagram(receiver); /* an 8 KiB message must not become 16 messages */
    for (unsigned pass = 0; pass < 3; pass++) {
        CHECK(sendto(sender, payload, sizeof(payload), 0, (void *)&destination,
                     sizeof(destination)) == sizeof(payload));
        ready(receiver, POLLIN);
        unsigned char prefix[21];
        memset(prefix, 0xa5, sizeof(prefix));
        struct iovec part = {prefix, 17};
        memset(&source, 0, sizeof(source));
        message = (struct msghdr){.msg_name = &source,
                                  .msg_namelen = 2,
                                  .msg_iov = &part,
                                  .msg_iovlen = 1};
        int flags = pass == 1 ? MSG_TRUNC : pass == 2 ? MSG_PEEK : 0;
        CHECK(recvmsg(receiver, &message, flags) ==
              (pass == 1 ? sizeof(payload) : 17));
        CHECK((message.msg_flags & MSG_TRUNC) &&
              message.msg_namelen == sizeof(struct sockaddr_in));
        CHECK(!memcmp(prefix, payload, 17) && prefix[17] == 0xa5 &&
              prefix[20] == 0xa5);
        if (pass == 2)
            CHECK(recv(receiver, received, sizeof(received), 0) ==
                  sizeof(payload));
        no_datagram(receiver);
    }
    CHECK(sendto(sender, NULL, 0, 0, (void *)&destination,
                 sizeof(destination)) == 0);
    ready(receiver, POLLIN);
    CHECK(recv(receiver, received, sizeof(received), 0) == 0);
    no_datagram(receiver);
    close(sender);
    close(receiver);
}
static void datagrams_v6(void) {
    int sender = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int receiver = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(sender >= 0 && receiver >= 0);
    struct sockaddr_in6 local = {.sin6_family = AF_INET6,
                                 .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    CHECK(bind(sender, (void *)&local, sizeof(local)) == 0);
    CHECK(bind(receiver, (void *)&local, sizeof(local)) == 0);
    struct sockaddr_in6 destination, source;
    socklen_t size = sizeof(destination);
    CHECK(getsockname(receiver, (void *)&destination, &size) == 0 &&
          size == sizeof(destination));
    CHECK(sendto(sender, payload, sizeof(payload), 0, (void *)&destination,
                 sizeof(destination)) == sizeof(payload));
    ready(receiver, POLLIN);
    size = sizeof(source);
    CHECK(recvfrom(receiver, received, sizeof(received), 0, (void *)&source,
                   &size) == sizeof(payload));
    CHECK(size == sizeof(source) && source.sin6_family == AF_INET6 &&
          source.sin6_port &&
          !memcmp(source.sin6_addr.s6_addr, local.sin6_addr.s6_addr, 16));
    CHECK(!memcmp(payload, received, sizeof(payload)));
    no_datagram(receiver);
    close(sender);
    close(receiver);
}
static void unix_datagrams(void) {
    const char *path = "/mlibc-net-dgram",
               *source_path = "/mlibc-net-dgram-src";
    unlink(path);
    unlink(source_path);
    int sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    CHECK(sender >= 0 && receiver >= 0);
    struct sockaddr_un destination = {.sun_family = AF_UNIX},
                       source = {.sun_family = AF_UNIX};
    strcpy(destination.sun_path, path);
    strcpy(source.sun_path, source_path);
    socklen_t destination_size =
        offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1;
    socklen_t source_size =
        offsetof(struct sockaddr_un, sun_path) + strlen(source_path) + 1;
    CHECK(bind(sender, (void *)&source, source_size) == 0 &&
          bind(receiver, (void *)&destination, destination_size) == 0);
    CHECK(sendto(sender, payload, sizeof(payload), 0, (void *)&destination,
                 destination_size) == sizeof(payload));
    ready(receiver, POLLIN);
    memset(&source, 0, sizeof(source));
    source_size = sizeof(source);
    CHECK(recvfrom(receiver, received, sizeof(received), 0, (void *)&source,
                   &source_size) == sizeof(payload));
    CHECK(source.sun_family == AF_UNIX &&
          !strcmp(source.sun_path, source_path));
    CHECK(!memcmp(payload, received, sizeof(payload)));
    no_datagram(receiver);
    close(sender);
    close(receiver);
    unlink(path);
    unlink(source_path);
}
static void streams(void) {
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    struct timeval timeout = {.tv_usec = 100000};
    CHECK(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    CHECK(send(pair[0], payload, sizeof(payload), 0) == sizeof(payload));
    struct iovec target = {received, sizeof(received)};
    struct msghdr header = {.msg_iov = &target, .msg_iovlen = 1};
    CHECK(recvmsg(pair[1], &header, 0) == sizeof(payload) &&
          !memcmp(received, payload, sizeof(payload)));
    CHECK(header.msg_controllen == 0 && header.msg_flags == 0);
    close(pair[0]);
    CHECK(recv(pair[1], received, sizeof(received), 0) == 0);
    close(pair[1]);
}
static void tcp(void) {
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener, (void *)&address, sizeof(address)) == 0 &&
          listen(listener, 8) == 0);
    socklen_t size = sizeof(address);
    CHECK(getsockname(listener, (void *)&address, &size) == 0 &&
          address.sin_port);
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(client >= 0);
    int connected = connect(client, (void *)&address, sizeof(address));
    CHECK(connected == 0 || (connected == -1 && errno == EINPROGRESS));
    ready(client, POLLOUT);
    int error = -1;
    size = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
          error == 0);
    ready(listener, POLLIN);
    struct sockaddr_in peer;
    size = sizeof(peer);
    int accepted =
        accept4(listener, (void *)&peer, &size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(accepted >= 0 && size == sizeof(peer));
    CHECK((fcntl(accepted, F_GETFL) & O_NONBLOCK) &&
          (fcntl(accepted, F_GETFD) & FD_CLOEXEC));
    CHECK(send(client, payload, sizeof(payload), MSG_NOSIGNAL) ==
          sizeof(payload));
    ready(accepted, POLLIN);
    size_t count = 0;
    while (count < sizeof(payload)) {
        ready(accepted, POLLIN);
        ssize_t part =
            recv(accepted, received + count, sizeof(payload) - count, 0);
        if (part == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(part > 0);
        count += part;
    }
    CHECK(!memcmp(payload, received, sizeof(payload)));
    CHECK(shutdown(client, SHUT_WR) == 0);
    ready(accepted, POLLIN);
    CHECK(recv(accepted, received, sizeof(received), 0) == 0);
    CHECK(shutdown(client, SHUT_WR) == 0);
    CHECK(shutdown(client, SHUT_RDWR) == 0);
    CHECK(shutdown(client, SHUT_RDWR) == 0);
    close(accepted);
    close(client);
    close(listener);
    client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(client >= 0);
    errno = 0;
    CHECK(connect(client, (void *)&address, sizeof(address)) == -1 &&
          (errno == ECONNREFUSED || errno == EINPROGRESS));
    if (errno == EINPROGRESS) {
        struct pollfd item = {client, POLLOUT, 0};
        CHECK(poll(&item, 1, 5000) == 1 &&
              (item.revents & (POLLOUT | POLLERR | POLLHUP)));
        size = sizeof(error);
        CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
              error == ECONNREFUSED);
    }
    close(client);
}
static void host_network(int port, int refused_port) {
    fprintf(stderr, "MLIBC_NETWORK: stage=host-connect\n");
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons(port)};
    CHECK(inet_pton(AF_INET, "10.0.2.2", &address.sin_addr) == 1);
    int client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(client >= 0);
    errno = 0;
    int result = connect(client, (void *)&address, sizeof(address));
    CHECK(result == 0 || (result == -1 && errno == EINPROGRESS));
    ready(client, POLLOUT);
    int error = -1;
    socklen_t size = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
          error == 0);
    const char request[] = "GET /native-mlibc HTTP/1.0\r\nHost: "
                           "10.0.2.2\r\nConnection: close\r\n\r\n";
    CHECK(send(client, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
          sizeof(request) - 1);
    fprintf(stderr, "MLIBC_NETWORK: stage=host-http\n");
    size_t total = 0;
    for (;;) {
        ready(client, POLLIN);
        ssize_t count =
            recv(client, received + total, sizeof(received) - total, 0);
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(count >= 0);
        if (!count)
            break;
        total += count;
        CHECK(total < sizeof(received));
    }
    CHECK(shutdown(client, SHUT_RDWR) == 0);
    close(client);
    received[total] = 0;
    CHECK(total > sizeof(payload) && !memcmp(received, "HTTP/1.0 200", 12));
    unsigned char *body = (void *)strstr((char *)received, "\r\n\r\n");
    CHECK(body && total - (body + 4 - received) == sizeof(payload));
    CHECK(!memcmp(body + 4, payload, sizeof(payload)));
    address.sin_port = htons(refused_port);
    client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(client >= 0);
    errno = 0;
    CHECK(connect(client, (void *)&address, sizeof(address)) == -1);
    if (errno == EINPROGRESS) {
        struct pollfd item = {client, POLLOUT, 0};
        CHECK(poll(&item, 1, 5000) == 1 &&
              (item.revents & (POLLOUT | POLLERR | POLLHUP)));
        size = sizeof(error);
        CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
              error == ECONNREFUSED);
        error = -1;
        CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
              error == 0);
    } else
        CHECK(errno == ECONNREFUSED);
    close(client);
    puts("MLIBC_NETWORK: host HTTP body=8192 connect refused SO_ERROR cleared");
}
int main(int argc, char **argv) {
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (i * 37u + 11u) & 255;
    fprintf(stderr, "MLIBC_NETWORK: stage=udp4\n");
    datagrams_v4();
    fprintf(stderr, "MLIBC_NETWORK: stage=udp6\n");
    datagrams_v6();
    fprintf(stderr, "MLIBC_NETWORK: stage=unix\n");
    unix_datagrams();
    fprintf(stderr, "MLIBC_NETWORK: stage=streams\n");
    streams();
    fprintf(stderr, "MLIBC_NETWORK: stage=tcp\n");
    tcp();
    if (argc == 4 && !strcmp(argv[1], "--host"))
        host_network(atoi(argv[2]), atoi(argv[3]));
    puts("MLIBC_NETWORK: PASS IPv4 IPv6 Unix UDP TCP scatter truncation peek "
         "zero-datagram EOF flags");
    return 0;
}
