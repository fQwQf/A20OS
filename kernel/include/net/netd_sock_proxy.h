#ifndef _NET_NETD_SOCK_PROXY_H
#define _NET_NETD_SOCK_PROXY_H

#include "core/types.h"

int netd_socket_create(int domain, int type, int protocol, int *netd_id_out);
int netd_socket_bind(int netd_id, const void *sockaddr, uint32_t addrlen);
int netd_socket_listen(int netd_id, int backlog);
int netd_socket_connect(int netd_id, const void *sockaddr, uint32_t addrlen,
                        int nonblock);
int netd_socket_accept(int netd_id, void *sockaddr, uint32_t *addrlen,
                       int nonblock, int *new_netd_id);
ssize_t netd_socket_send(int netd_id, const void *buf, size_t len, int flags,
                         const void *dst, uint32_t dst_len);
ssize_t netd_socket_recv(int netd_id, void *buf, size_t len, int flags,
                         void *src, uint32_t *src_len);
int netd_socket_close(int netd_id);
int netd_socket_poll(int netd_id, uint32_t events);
int netd_socket_getsockname(int netd_id, void *sockaddr, uint32_t *addrlen);
int netd_socket_getpeername(int netd_id, void *sockaddr, uint32_t *addrlen);
int netd_socket_setsockopt(int netd_id, int level, int optname,
                           const void *optval, uint32_t optlen);

#endif
