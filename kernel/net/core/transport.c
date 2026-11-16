/* Frontend -> backend socket dispatch.
 *
 * The frontend never sees a protocol control block.  Each helper resolves the
 * selected backend's module once and degrades to a precise error when the
 * module or the advertised capability is absent.  This is the only file that
 * maps the socket-facing part of net/backend.h onto the old internal helpers.
 */
#include "net/socket/socket_internal.h"
#include "net/backend.h"
#include "net/stack.h"
#include "core/errno.h"

static const net_socket_ops_t *sock_ops(void) {
    const net_backend_t *b = net_backend_current();
    return (b && b->socket) ? b->socket : NULL;
}

static const net_socket_opt_ops_t *opt_ops(void) {
    const net_backend_t *b = net_backend_current();
    return (b && b->opts) ? b->opts : NULL;
}

static const net_endpoint_ops_t *endpoint_ops(void) {
    const net_backend_t *b = net_backend_current();
    return (b && b->endpoint) ? b->endpoint : NULL;
}

int net_inet_socket_init(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (!ops || !ops->open)
        return -ENETDOWN;
    return ops->open(s);
}

void net_inet_socket_destroy(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->close)
        ops->close(s);
}

void net_inet_state_release(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->release)
        ops->release(s);
}

int net_inet_bind_pcb(net_socket_t *s, const void *a, size_t n) {
    const net_socket_ops_t *ops = sock_ops();
    return (ops && ops->bind) ? ops->bind(s, a, n) : -EOPNOTSUPP;
}

int net_inet_connect(net_socket_t *s, const void *a, size_t n, const void *p,
                     size_t pn) {
    const net_socket_ops_t *ops = sock_ops();
    return (ops && ops->connect) ? ops->connect(s, a, n, p, pn) : -EOPNOTSUPP;
}

int net_inet_tcp_listen(net_socket_t *s, int backlog) {
    const net_socket_ops_t *ops = sock_ops();
    if (!net_backend_has(NET_CAP_LISTEN))
        return -EOPNOTSUPP;
    return (ops && ops->listen) ? ops->listen(s, backlog) : -EOPNOTSUPP;
}

int net_inet_sendto(net_socket_t *s, const void *b, size_t n, int f,
                    const void *a, size_t an) {
    const net_socket_ops_t *ops = sock_ops();
    return (ops && ops->send) ? ops->send(s, b, n, f, a, an) : -EOPNOTSUPP;
}

int net_inet_shutdown(net_socket_t *s, int receive, int transmit) {
    const net_socket_ops_t *ops = sock_ops();
    if (!net_backend_has(NET_CAP_SHUTDOWN))
        return -EOPNOTSUPP;
    return (ops && ops->shutdown) ? ops->shutdown(s, receive, transmit)
                                  : -EOPNOTSUPP;
}

void net_inet_accept_child_ready(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->accept_ready)
        ops->accept_ready(s);
}

void net_tcp_recved(net_socket_t *s, size_t n) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->recved)
        ops->recved(s, n);
}

void net_inet_options_apply(net_socket_t *s, unsigned mask) {
    const net_socket_opt_ops_t *ops = opt_ops();
    if (ops && ops->apply)
        ops->apply(s, mask);
}

int net_inet_option_validate(net_socket_t *s, int level, int option,
                             const void *v, size_t n) {
    const net_socket_opt_ops_t *ops = opt_ops();
    return (ops && ops->validate) ? ops->validate(s, level, option, v, n) : 0;
}

uint32_t net_inet_buffer_default(int send) {
    const net_socket_opt_ops_t *ops = opt_ops();
    return (ops && ops->buf_default) ? ops->buf_default(send) : 0;
}

int net_inet_has_transport(const net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (!ops)
        return 0;
    if (ops->has_transport)
        return ops->has_transport(s);
    return s != NULL;
}

int net_inet_has_tcp(const net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->has_stream)
        return ops->has_stream(s);
    return net_backend_has(NET_CAP_STREAM) && s != NULL;
}

int net_inet_bind_device(net_socket_t *s, const char *name) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    if (!net_backend_has(NET_CAP_BINDTODEVICE))
        return -EOPNOTSUPP;
    return (ops && ops->bind_device) ? ops->bind_device(s, name) : -EOPNOTSUPP;
}

int net_inet_bound_device(net_socket_t *s, char name[16]) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    if (!net_backend_has(NET_CAP_BINDTODEVICE))
        return -EOPNOTSUPP;
    return (ops && ops->bound_device) ? ops->bound_device(s, name)
                                      : -EOPNOTSUPP;
}

int net_inet_send_udp_control(net_socket_t *s, const void *b, size_t n, int f,
                              const void *a, size_t an,
                              const net_datagram_control_t *c) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    if (!net_backend_has(NET_CAP_UDP_CONTROL))
        return -EOPNOTSUPP;
    return (ops && ops->udp_control) ? ops->udp_control(s, b, n, f, a, an, c)
                                     : -EOPNOTSUPP;
}

int net_inet_recverr(net_socket_t *s, int enable) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    return (ops && ops->recverr) ? ops->recverr(s, enable) : -EOPNOTSUPP;
}

int net_inet_pmtu_discover(net_socket_t *s, int mode, int set) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    if (!net_backend_has(NET_CAP_PMTU))
        return -EOPNOTSUPP;
    return (ops && ops->pmtu) ? ops->pmtu(s, mode, set) : -EOPNOTSUPP;
}

int net_inet_send_raw_packet(net_socket_t *s, const void *b, size_t n,
                             const void *a, size_t an, int h, int t) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    if (!net_backend_has(NET_CAP_RAW_TX))
        return -EOPNOTSUPP;
    return (ops && ops->raw_send) ? ops->raw_send(s, b, n, a, an, h, t)
                                  : -EOPNOTSUPP;
}

int net_inet_interface_ioctl(net_socket_t *s, unsigned long request, void *a) {
    const net_endpoint_ops_t *ops = endpoint_ops();
    return (ops && ops->ioctl) ? ops->ioctl(s, request, a) : -EOPNOTSUPP;
}

unsigned net_inet_transport_flags(const net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    return (ops && ops->flags) ? ops->flags(s) : 0;
}

void net_tcp_close_pcb(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->close_pcb)
        ops->close_pcb(s);
}

void net_tcp_drop_pcb(net_socket_t *s) {
    const net_socket_ops_t *ops = sock_ops();
    if (ops && ops->drop_pcb)
        ops->drop_pcb(s);
}

uint32_t net_socket_buf_ceiling(net_socket_t *s, int is_snd) {
    const net_socket_opt_ops_t *ops = opt_ops();
    return (ops && ops->buf_ceiling) ? ops->buf_ceiling(s, is_snd) : 0;
}

uint32_t net_socket_buf_in_force(net_socket_t *s, int is_snd) {
    const net_socket_opt_ops_t *ops = opt_ops();
    return (ops && ops->buf_in_force) ? ops->buf_in_force(s, is_snd) : 0;
}

void net_inet_ip_opts_apply(net_socket_t *s) {
    const net_socket_opt_ops_t *ops = opt_ops();
    if (ops && ops->ip_apply)
        ops->ip_apply(s);
}

void net_inet_ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                           uint8_t *mc_ttl) {
    const net_socket_opt_ops_t *ops = opt_ops();
    if (ops && ops->ip_effective) {
        ops->ip_effective(s, ttl, tos, mc_ttl);
        return;
    }
    if (ttl)
        *ttl = 64;
    if (tos)
        *tos = 0;
    if (mc_ttl)
        *mc_ttl = 1;
}
