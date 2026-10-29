/* Backend socket operations. Only value arguments and borrowed buffers cross
 * this boundary; transport_private belongs to the selected backend until the
 * final frontend reference. No protocol PCB or allocator type is public. */
#ifndef A20_NET_TRANSPORT_H
#define A20_NET_TRANSPORT_H
#include "core/types.h"
#include "net/datagram.h"
struct net_socket;
typedef struct net_transport_ops {
    int (*socket_init)(struct net_socket *s);
    void (*socket_destroy)(struct net_socket *s);
    void (*state_release)(struct net_socket *s);
    int (*bind_pcb)(struct net_socket *s, const void *a, size_t n);
    int (*connect)(struct net_socket *s, const void *a, size_t n, const void *p, size_t pn);
    int (*tcp_listen)(struct net_socket *s, int backlog);
    int (*sendto)(struct net_socket *s, const void *b, size_t n, int f, const void *a, size_t an);
    int (*shutdown)(struct net_socket *s, int receive, int transmit);
    void (*accept_child_ready)(struct net_socket *s);
    void (*recved)(struct net_socket *s, size_t n);
    void (*options_apply)(struct net_socket *s, unsigned mask);
    int (*option_validate)(struct net_socket *s, int level, int option, const void *v, size_t n);
    uint32_t (*buffer_default)(int send);
    int (*has_transport)(const struct net_socket *s);
    int (*has_tcp)(const struct net_socket *s);
    int (*bind_device)(struct net_socket *s, const char *name);
    int (*bound_device)(struct net_socket *s, char name[16]);
    int (*send_udp_control)(struct net_socket *s, const void *b, size_t n, int f, const void *a, size_t an, const net_datagram_control_t *c);
    int (*pmtu_discover)(struct net_socket *s, int mode, int set);
    int (*send_raw_packet)(struct net_socket *s, const void *b, size_t n, const void *a, size_t an, int h, int t);
    int (*interface_ioctl)(struct net_socket *s, unsigned long request, void *a);
    unsigned (*transport_flags)(const struct net_socket *s);
    void (*close_pcb)(struct net_socket *s);
    void (*drop_pcb)(struct net_socket *s);
} net_transport_ops_t;
#endif
