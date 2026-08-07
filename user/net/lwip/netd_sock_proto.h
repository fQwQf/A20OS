/*
 * netd socket RPC protocol: kernel socket proxy <-> netd (userspace lwIP).
 *
 * Each operation is one channel message pair: request (hdr + payload) from
 * the kernel, response (hdr + payload) from netd.  Blocking operations
 * (accept, connect, recv) may be held by netd until the condition is met.
 * Sockaddrs travel as raw bytes (NET_SOCKADDR_MAX).
 */
#ifndef _NETD_SOCK_PROTO_H
#define _NETD_SOCK_PROTO_H

#include <stdint.h>

#define NETD_SOCK_MAX_ADDR   128
#define NETD_SOCK_MAX_DATA   61440
#define NETD_SOCK_MAX_PCB    256

typedef struct netd_sock_hdr {
    uint32_t op;
    int32_t  id;          /* netd-side connection id */
    int32_t  status;      /* response: 0 or negative errno */
    uint32_t len;         /* payload length */
    uint32_t flags;       /* request flags (e.g. NETD_SOCK_F_NONBLOCK) */
} netd_sock_hdr_t;

#define NETD_SOCK_F_NONBLOCK 1u

enum {
    NETD_SOCK_OP_CREATE = 1,
    NETD_SOCK_OP_BIND,
    NETD_SOCK_OP_LISTEN,
    NETD_SOCK_OP_CONNECT,
    NETD_SOCK_OP_ACCEPT,
    NETD_SOCK_OP_SEND,
    NETD_SOCK_OP_RECV,
    NETD_SOCK_OP_CLOSE,
    NETD_SOCK_OP_GETSOCKNAME,
    NETD_SOCK_OP_GETPEERNAME,
    NETD_SOCK_OP_GETSOCKOPT,
    NETD_SOCK_OP_SETSOCKOPT,
    NETD_SOCK_OP_SHUTDOWN,
    NETD_SOCK_OP_POLL,
    NETD_SOCK_OP_SENDTO,
    NETD_SOCK_OP_RECVFROM,
};

/* CREATE payload: domain, type, protocol */
typedef struct netd_sock_create_req {
    int32_t domain;
    int32_t type;
    int32_t protocol;
} netd_sock_create_req_t;

/* BIND/LISTEN/CONNECT/SENDTO payload: addr + port (network order in addr). */
typedef struct netd_sock_addr {
    uint16_t family;      /* AF_INET / AF_INET6 */
    uint16_t port;        /* network byte order */
    uint8_t  addr[16];    /* IPv4 in first 4 bytes */
} netd_sock_addr_t;

/* SEND payload: addr (optional, SENDTO) + data */
typedef struct netd_sock_send_req {
    uint32_t data_len;
    netd_sock_addr_t addr;   /* family=0 for plain SEND */
    uint8_t  data[];
} netd_sock_send_req_t;

/* RECVFROM response payload: addr (optional) + data */
typedef struct netd_sock_recv_resp {
    uint32_t data_len;
    netd_sock_addr_t addr;   /* family=0 when not requested */
    uint8_t  data[];
} netd_sock_recv_resp_t;

/* GETSOCKOPT/SETSOCKOPT payload: level, optname, value */
typedef struct netd_sock_opt_req {
    int32_t level;
    int32_t optname;
    uint32_t optlen;
    uint8_t  optval[];
} netd_sock_opt_req_t;

/* POLL payload: events (POLLIN/POLLOUT bits) */
typedef struct netd_sock_poll_req {
    uint32_t events;
} netd_sock_poll_req_t;

#endif
