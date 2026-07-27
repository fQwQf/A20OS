#ifndef _NET_SOCKET_INTERNAL_H
#define _NET_SOCKET_INTERNAL_H

#include "net/socket.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/timer.h"
#include "lwip/ip_addr.h"

struct udp_pcb;
struct raw_pcb;
struct tcp_pcb;

#define NET_MAX_SOCKETS 1024
#define NET_MAX_STREAM_PAYLOAD 2048
#define NET_MAX_PAYLOAD 65535
#define NET_MAX_QUEUE   128
#define NET_CONNECT_TIMEOUT_TICKS MS_TO_TICKS(10000)
#define NET_BH_RING_SIZE 16

typedef enum {
    NET_BH_RECV = 0,
    NET_BH_CONNECTED,
    NET_BH_CLOSED,
    NET_BH_ERROR,
} net_bh_event_type_t;

typedef struct net_bh_event {
    struct net_bh_event *next;
    net_bh_event_type_t type;
    int err;
    size_t len;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    uint8_t data[NET_MAX_PAYLOAD];
} net_bh_event_t;

typedef struct net_bh_ring {
    net_bh_event_t events[NET_BH_RING_SIZE];
    uint32_t head;
    uint32_t tail;
} net_bh_ring_t;

typedef struct net_msg {
    struct net_msg *next;
    size_t len;
    size_t off;
    uint8_t addr[NET_SOCKADDR_MAX];
    size_t addrlen;
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
    uint8_t data[NET_MAX_PAYLOAD];
} net_msg_t;

typedef struct net_recv_meta {
    uint8_t has_pktinfo;
    uint8_t has_hoplimit;
    uint8_t has_tclass;
    uint8_t pad;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    uint8_t hoplimit;
    uint8_t tclass;
    uint16_t __pad_meta;
} net_recv_meta_t;

typedef struct net_socket {
    int domain;
    int type;
    int protocol;
    int nonblock;
    int closed;
    int shut_rd;
    int shut_wr;
    int peer_closed;
    int bound;
    int connected;
    int listening;
    uint8_t local[NET_SOCKADDR_MAX];
    size_t local_len;
    uint8_t peer_addr[NET_SOCKADDR_MAX];
    size_t peer_len;
    struct net_socket *peer;
    net_msg_t *rx_head;
    net_msg_t *rx_tail;
    int rx_count;
    wait_queue_t accept_waitq;
    wait_queue_t read_waitq;
    wait_queue_t write_waitq;
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    struct tcp_pcb *tcp;
    int local_tcp;
    int tcp_connecting;
    int tcp_err;
    int tcp_nodelay;
    int reuseaddr;
    int reuseport;
    int keepalive;
    int keep_idle;
    int keep_intvl;
    int keep_cnt;
    uint64_t recv_timeout_ticks;
    uint64_t send_timeout_ticks;
    int ipv6_checksum_offset;
    uint32_t icmp6_filter[8];
    int icmp6_filter_set;
    int ipv6_recv_pktinfo;
    int ipv6_recv_tclass;
    int ipv6_recv_hoplimit;
    int ipv6_recv_rthdr;
    int ipv6_recv_hopopts;
    int ipv6_recv_dstopts;
    int ipv6_recv_err;
    int ipv6_recv_2292_pktinfo;
    int ipv6_recv_2292_hoplimit;
    int ipv6_recv_2292_rthdr;
    int ipv6_recv_2292_hopopts;
    int ipv6_recv_2292_dstopts;
    int bpf_prog_fd;
    uint8_t alg_last[NET_MAX_STREAM_PAYLOAD];
    size_t alg_last_len;
    char alg_type[16];
    char alg_name[64];
    struct net_socket *accept_next;
    struct net_socket *accept_head;
    struct net_socket *accept_tail;
    int accept_count;
    int in_registry;
    int reg_idx;
    net_bh_ring_t bh_ring;
    volatile int bh_connected;
    volatile int bh_closed;
    volatile int bh_error;
    volatile int bh_err_code;
    volatile int bh_tx_wake;
    volatile int bh_pending;
} net_socket_t;

typedef struct sockaddr_alg_kernel {
    uint16_t family;
    uint8_t type[14];
    uint32_t feat;
    uint32_t mask;
    uint8_t name[64];
} sockaddr_alg_kernel_t;

extern spinlock_t g_net_lock;
extern net_socket_t *g_sockets[NET_MAX_SOCKETS];
extern volatile int g_net_bh_pending[NET_MAX_SOCKETS];

/*
 * Return true when the bounded deferred-wake batch filled before the queue
 * was drained.  The caller must drop g_net_lock, flush wake_q, and finish the
 * queue with wait_queue_wake_all().
 */
static inline bool
net_wait_queue_collect_all_locked(wait_queue_t *q,
                                  proc_wake_reason_t reason,
                                  proc_wake_q_t *wake_q)
{
    bool complete = false;
    (void)wait_queue_collect_all(q, 0, reason, wake_q, &complete);
    return !complete;
}

void     net_socket_registry_init(void);
uint16_t net_alloc_ephemeral_port_locked(void);
uint16_t net_ntohs(uint16_t x);
void     net_sockaddr_loopback(net_socket_t *s, uint16_t port);
int      net_sockaddr_port(const void *addr, size_t len, uint16_t *port);
void     net_sockaddr_set_port(void *addr, size_t len, uint16_t port);
int      net_sockaddr_in_local(const net_sockaddr_in_t *in);
int      net_sockaddr_to_lwip_ip(const void *addr, size_t len,
                                 ip_addr_t *ip, uint16_t *port);
int      net_lwip_ip_to_sockaddr(const ip_addr_t *ip, uint16_t port,
                                 uint8_t out[NET_SOCKADDR_MAX],
                                 size_t *outlen);
net_socket_t *net_find_bound_socket_locked(int domain, int type,
                                           const void *addr, size_t addrlen);
int      net_register_socket_locked(net_socket_t *s);
void     net_unregister_socket_locked(net_socket_t *s);
int      net_socket_is_valid_locked(net_socket_t *s);

int      net_enqueue_msg_locked(net_socket_t *dst, const void *buf, size_t len,
                                const void *addr, size_t addrlen);
int      net_enqueue_msg_locked_meta(net_socket_t *dst, const void *buf, size_t len,
                                     const void *addr, size_t addrlen,
                                     const net_bh_event_t *meta);
int      net_enqueue_msg_blocking(net_socket_t *s, net_socket_t *dst, const void *buf, size_t len,
                                  const void *addr, size_t addrlen,
                                  int dontwait, uint64_t timeout_ticks);
int      net_dequeue_msg_locked(net_socket_t *s, void *buf, size_t len,
                                void *addr, size_t *addrlen);
int      net_dequeue_msg_locked_meta(net_socket_t *s, void *buf, size_t len,
                                     void *addr, size_t *addrlen,
                                     net_recv_meta_t *meta);
int      net_recvfrom_meta(int gfd, void *buf, size_t len, int flags,
                           void *addr, size_t *addrlen, net_recv_meta_t *meta);
int      net_accept_queue_push_locked(net_socket_t *listener,
                                      net_socket_t *child);
net_socket_t *net_accept_queue_pop_locked(net_socket_t *listener);
net_msg_t    *net_msg_alloc(void);
void          net_msg_free(net_msg_t *m);
net_socket_t *net_socket_alloc(void);
void          net_socket_free(net_socket_t *s);

int      net_task_has_unblocked_signal(task_t *t);
int      net_socket_wait_expired(net_socket_t *s, uint64_t start, int for_write);

void     net_alg_copy_string(char *dst, size_t dstsz,
                             const uint8_t *src, size_t srcsz);
int      net_alg_name_supported(const char *type, const char *name);
int      net_alg_socket_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_alg_socket_accept(net_socket_t *s, size_t *addrlen, int flags);
int      net_alg_socket_send(net_socket_t *s, const void *buf, size_t len);
int      net_alg_socket_recv(net_socket_t *s, void *buf, size_t len);

int      net_unix_sockaddr_prepare(const void *addr, size_t addrlen,
                                   uint8_t out[NET_SOCKADDR_MAX],
                                   size_t *outlen,
                                   char *path_out, size_t path_outsz);
int      net_unix_path_parent_ok(const char *path);
int      net_unix_socket_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_unix_socket_connect(net_socket_t *s, const void *addr, size_t addrlen);
int      net_unix_socket_sendto(net_socket_t *s, const void *buf, size_t len,
                                const void *addr, size_t addrlen);
int      net_netlink_bind(net_socket_t *s, const void *addr, size_t addrlen);
int      net_netlink_diag_request(net_socket_t *s, const void *buf, size_t len,
                                  const void *addr, size_t addrlen);

void     net_tcp_close_pcb(net_socket_t *s);
void     net_tcp_drop_pcb(net_socket_t *s);
int      net_inet_socket_init(net_socket_t *s);
void     net_inet_socket_destroy(net_socket_t *s);
void     net_inet_bottom_half_process_socket(net_socket_t *s);
void     net_inet_bottom_half_process_all(void);
int      net_inet_bind_pcb(net_socket_t *s, const void *addr, size_t addrlen);
int      net_inet_connect(net_socket_t *s, const void *addr, size_t addrlen,
                          const void *connect_addr, size_t peer_len);
int      net_inet_sendto(net_socket_t *s, const void *buf, size_t len,
                         int flags, const void *addr, size_t addrlen);
void     net_inet_accept_child_ready(net_socket_t *s);

net_socket_t *net_socket_from_file(int gfd);
int      net_socket_install_file(net_socket_t *s, int flags);
int      net_socket_close_file(vfile_t *vf);

#endif /* _NET_SOCKET_INTERNAL_H */

void net_tcp_recved(net_socket_t *s, size_t len);
