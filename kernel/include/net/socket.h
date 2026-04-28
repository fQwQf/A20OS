#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include "core/types.h"

#define AF_UNIX    1
#define AF_INET    2
#define AF_INET6   10
#define AF_ALG     38

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
#define SOCK_SEQPACKET 5

#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000

#define SOL_SOCKET   1
#define SOL_ALG      279
#define IPPROTO_IP   0
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_ATTACH_BPF 50

#define MCAST_JOIN_GROUP  42
#define MCAST_LEAVE_GROUP 45

#define ALG_SET_KEY  1

#define NET_SOCKADDR_MAX 128

typedef struct net_sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} net_sockaddr_t;

typedef struct net_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} net_sockaddr_in_t;

void net_init(void);
int  net_format_status(char *buf, size_t bufsz);

int net_socket_create(int domain, int type, int protocol);
int net_socketpair_create(int domain, int type, int protocol, int out_gfd[2]);
int net_bind(int gfd, const void *addr, size_t addrlen);
int net_connect(int gfd, const void *addr, size_t addrlen);
int net_listen(int gfd, int backlog);
int net_accept(int gfd, void *addr, size_t *addrlen, int flags);
int net_getsockname(int gfd, void *addr, size_t *addrlen);
int net_getpeername(int gfd, void *addr, size_t *addrlen);
int net_sendto(int gfd, const void *buf, size_t len, int flags,
               const void *addr, size_t addrlen);
int net_recvfrom(int gfd, void *buf, size_t len, int flags,
                 void *addr, size_t *addrlen);
int net_setsockopt(int gfd, int level, int optname, const void *optval, size_t optlen);
int net_getsockopt(int gfd, int level, int optname, void *optval, size_t *optlen);
int net_shutdown(int gfd, int how);
int net_set_nonblock(int gfd, int nonblock);
int net_poll_events(int gfd, short events);

#endif /* _NET_SOCKET_H */
