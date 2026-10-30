#include "native.h"
#include "net/transport.h"

extern int a20_lwip_transport_socket_init(net_socket_t *s);
extern void a20_lwip_transport_socket_destroy(net_socket_t *s);
extern void a20_lwip_transport_state_release(net_socket_t *s);
extern int a20_lwip_transport_bind_pcb(net_socket_t *s, const void *a, size_t n);
extern int a20_lwip_transport_connect(net_socket_t *s, const void *a, size_t n, const void *p, size_t pn);
extern int a20_lwip_transport_tcp_listen(net_socket_t *s, int backlog);
extern int a20_lwip_transport_sendto(net_socket_t *s, const void *b, size_t n, int f, const void *a, size_t an);
extern int a20_lwip_transport_shutdown(net_socket_t *s, int receive, int transmit);
extern void a20_lwip_transport_accept_child_ready(net_socket_t *s);
extern void a20_lwip_transport_recved(net_socket_t *s, size_t n);
extern void a20_lwip_transport_options_apply(net_socket_t *s, unsigned mask);
extern int a20_lwip_transport_option_validate(net_socket_t *s, int level, int option, const void *v, size_t n);
extern uint32_t a20_lwip_transport_buffer_default(int send);
extern int a20_lwip_transport_has_transport(const net_socket_t *s);
extern int a20_lwip_transport_has_tcp(const net_socket_t *s);
extern int a20_lwip_transport_bind_device(net_socket_t *s, const char *name);
extern int a20_lwip_transport_bound_device(net_socket_t *s, char name[16]);
extern int a20_lwip_transport_send_udp_control(net_socket_t *s, const void *b, size_t n, int f, const void *a, size_t an, const net_datagram_control_t *c);
extern int a20_lwip_transport_pmtu_discover(net_socket_t *s, int mode, int set);
extern int a20_lwip_transport_send_raw_packet(net_socket_t *s, const void *b, size_t n, const void *a, size_t an, int h, int t);
extern int a20_lwip_transport_interface_ioctl(net_socket_t *s, unsigned long request, void *a);
extern unsigned a20_lwip_transport_flags(const net_socket_t *s);
extern void a20_lwip_transport_close_pcb(net_socket_t *s);
extern void a20_lwip_transport_drop_pcb(net_socket_t *s);
extern uint32_t a20_lwip_transport_buffer_ceiling(net_socket_t *s, int is_snd);
extern uint32_t a20_lwip_transport_buffer_in_force(net_socket_t *s, int is_snd);
extern void a20_lwip_transport_ip_options_apply(net_socket_t *s);
extern void a20_lwip_transport_ip_effective(net_socket_t *s, uint8_t *ttl, uint8_t *tos,
                                            uint8_t *mc_ttl);

const net_socket_ops_t a20_lwip_socket_ops = {
    .open = a20_lwip_transport_socket_init,
    .close = a20_lwip_transport_socket_destroy,
    .release = a20_lwip_transport_state_release,
    .bind = a20_lwip_transport_bind_pcb,
    .connect = a20_lwip_transport_connect,
    .listen = a20_lwip_transport_tcp_listen,
    .send = a20_lwip_transport_sendto,
    .shutdown = a20_lwip_transport_shutdown,
    .accept_ready = a20_lwip_transport_accept_child_ready,
    .recved = a20_lwip_transport_recved,
    .close_pcb = a20_lwip_transport_close_pcb,
    .drop_pcb = a20_lwip_transport_drop_pcb,
    .has_transport = a20_lwip_transport_has_transport,
    .has_stream = a20_lwip_transport_has_tcp,
    .flags = a20_lwip_transport_flags,
};

const net_socket_opt_ops_t a20_lwip_socket_opt_ops = {
    .apply = a20_lwip_transport_options_apply,
    .validate = a20_lwip_transport_option_validate,
    .buf_default = a20_lwip_transport_buffer_default,
    .buf_ceiling = a20_lwip_transport_buffer_ceiling,
    .buf_in_force = a20_lwip_transport_buffer_in_force,
    .ip_apply = a20_lwip_transport_ip_options_apply,
    .ip_effective = a20_lwip_transport_ip_effective,
};

const net_endpoint_ops_t a20_lwip_endpoint_ops = {
    .bind_device = a20_lwip_transport_bind_device,
    .bound_device = a20_lwip_transport_bound_device,
    .udp_control = a20_lwip_transport_send_udp_control,
    .pmtu = a20_lwip_transport_pmtu_discover,
    .raw_send = a20_lwip_transport_send_raw_packet,
    .ioctl = a20_lwip_transport_interface_ioctl,
};
