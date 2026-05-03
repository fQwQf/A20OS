#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include "core/types.h"

#define AF_UNSPEC  0
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

#define MSG_DONTWAIT   0x0040
#define MSG_NOSIGNAL   0x4000
#define MSG_WAITFORONE 0x10000

#define SOL_SOCKET   1
#define SOL_ALG      279
#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define SOL_TCP      IPPROTO_TCP
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_LINGER    13
#define SO_REUSEPORT 15
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL  38
#define SO_DOMAIN    39
#define SO_ATTACH_BPF 50
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define TCP_NODELAY  1
#define TCP_MAXSEG   2
#define TCP_CORK     3
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT  6
#define TCP_SYNCNT   7
#define TCP_LINGER2  8
#define TCP_DEFER_ACCEPT 9
#define TCP_WINDOW_CLAMP 10
#define TCP_INFO     11
#define TCP_QUICKACK 12
#define TCP_CONGESTION 13
#define TCP_USER_TIMEOUT 18

#define ICMP6_FILTER   1
#define IPV6_CHECKSUM  7
#define IPV6_V6ONLY    26

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

typedef struct net_sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t  sin6_addr[16];
    uint32_t sin6_scope_id;
} net_sockaddr_in6_t;

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
