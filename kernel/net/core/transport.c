#include "net/socket/socket_internal.h"
#include "net/transport.h"
#include "net/stack.h"

int net_inet_socket_init(net_socket_t *s) {
    return net_stack_transport()->socket_init(s);
}
void net_inet_socket_destroy(net_socket_t *s) {
    net_stack_transport()->socket_destroy(s);
}
void net_inet_state_release(net_socket_t *s) {
    net_stack_transport()->state_release(s);
}
int net_inet_bind_pcb(net_socket_t *s, const void *a, size_t n) {
    return net_stack_transport()->bind_pcb(s,a,n);
}
int net_inet_connect(net_socket_t *s, const void *a, size_t n, const void *p, size_t pn) {
    return net_stack_transport()->connect(s,a,n,p,pn);
}
int net_inet_tcp_listen(net_socket_t *s, int backlog) {
    return net_stack_transport()->tcp_listen(s,backlog);
}
int net_inet_sendto(net_socket_t *s, const void *b, size_t n, int f, const void *a, size_t an) {
    return net_stack_transport()->sendto(s,b,n,f,a,an);
}
int net_inet_shutdown(net_socket_t *s, int receive, int transmit) {
    return net_stack_transport()->shutdown(s,receive,transmit);
}
void net_inet_accept_child_ready(net_socket_t *s) {
    net_stack_transport()->accept_child_ready(s);
}
void net_tcp_recved(net_socket_t *s, size_t n) {
    net_stack_transport()->recved(s,n);
}
void net_inet_options_apply(net_socket_t *s, unsigned mask) {
    net_stack_transport()->options_apply(s,mask);
}
int net_inet_option_validate(net_socket_t *s, int level, int option, const void *v, size_t n) {
    return net_stack_transport()->option_validate(s,level,option,v,n);
}
uint32_t net_inet_buffer_default(int send) {
    return net_stack_transport()->buffer_default(send);
}
int net_inet_has_transport(const net_socket_t *s) {
    return net_stack_transport()->has_transport(s);
}
int net_inet_has_tcp(const net_socket_t *s) {
    return net_stack_transport()->has_tcp(s);
}
int net_inet_bind_device(net_socket_t *s, const char *name) {
    return net_stack_transport()->bind_device(s,name);
}
int net_inet_bound_device(net_socket_t *s, char name[16]) {
    return net_stack_transport()->bound_device(s,name);
}
int net_inet_send_udp_control(net_socket_t *s, const void *b, size_t n, int f, const void *a, size_t an, const net_datagram_control_t *c) {
    return net_stack_transport()->send_udp_control(s,b,n,f,a,an,c);
}
int net_inet_pmtu_discover(net_socket_t *s, int mode, int set) {
    return net_stack_transport()->pmtu_discover(s,mode,set);
}
int net_inet_send_raw_packet(net_socket_t *s, const void *b, size_t n, const void *a, size_t an, int h, int t) {
    return net_stack_transport()->send_raw_packet(s,b,n,a,an,h,t);
}
int net_inet_interface_ioctl(net_socket_t *s, unsigned long request, void *a) {
    return net_stack_transport()->interface_ioctl(s,request,a);
}
unsigned net_inet_transport_flags(const net_socket_t *s) {
    return net_stack_transport()->transport_flags(s);
}
void net_tcp_close_pcb(net_socket_t *s) {
    net_stack_transport()->close_pcb(s);
}
void net_tcp_drop_pcb(net_socket_t *s) {
    net_stack_transport()->drop_pcb(s);
}
