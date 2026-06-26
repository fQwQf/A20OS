#include "net/socket_internal.h"
#include "net/lwip_stack.h"

#include "core/string.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"

extern void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port);
extern u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr);
extern err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err);
extern err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                              err_t err);
extern void lwip_tcp_err_cb(void *arg, err_t err);
extern err_t lwip_tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len);

void a20_udp_recv_register_rust(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg)
{
    udp_recv(pcb, recv ? lwip_udp_recv_cb : NULL, recv ? recv_arg : NULL);
}

void a20_raw_recv_register_rust(struct raw_pcb *pcb, raw_recv_fn recv, void *recv_arg)
{
    raw_recv(pcb, recv ? lwip_raw_recv_cb : NULL, recv ? recv_arg : NULL);
}

void a20_tcp_recv_register_rust(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
    tcp_recv(pcb, recv ? lwip_tcp_recv_cb : NULL);
}

void a20_tcp_err_register_rust(struct tcp_pcb *pcb, tcp_err_fn err)
{
    tcp_err(pcb, err ? lwip_tcp_err_cb : NULL);
}

void a20_tcp_sent_register_rust(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    tcp_sent(pcb, sent ? lwip_tcp_sent_cb : NULL);
}

err_t a20_tcp_connect_with_rust_cb(struct tcp_pcb *pcb, const ip_addr_t *ipaddr,
                                   u16_t port, tcp_connected_fn connected)
{
    return tcp_connect(pcb, ipaddr, port, connected ? lwip_tcp_connected_cb : NULL);
}

int a20_socket_bh_task_state(task_t *task)
{
    return task ? (int)task->state : 0;
}

void a20_socket_bh_proc_make_ready(task_t *task)
{
    if (task)
        proc_make_ready(task);
}

uint32_t a20_socket_bh_atomic_load_u32_relaxed(const uint32_t *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

uint32_t a20_socket_bh_atomic_load_u32_acquire(const uint32_t *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

uint32_t a20_socket_bh_atomic_fetch_add_u32_relaxed(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_RELAXED);
}

uint32_t a20_socket_bh_atomic_fetch_add_u32_release(uint32_t *ptr, uint32_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_RELEASE);
}

void a20_socket_bh_atomic_thread_fence_release(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void a20_socket_bh_atomic_thread_fence_acquire(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void a20_socket_bh_atomic_store_int_release(int *ptr, int val)
{
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

int a20_socket_bh_atomic_load_int_acquire(const int *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

int a20_socket_bh_atomic_load_int_relaxed(const int *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
}

int a20_socket_bh_atomic_exchange_int_acquire(int *ptr, int val)
{
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQUIRE);
}

int a20_socket_bh_ip_current_is_v6(void)
{
    return ip_current_is_v6();
}

uint32_t a20_socket_bh_ip_current_input_ifindex(void)
{
    return ip_current_input_netif() ? (uint32_t)netif_get_index(ip_current_input_netif()) : 0;
}

void a20_socket_bh_ip6_current_dest_addr_copy(uint8_t *out)
{
    if (!out)
        return;
    if (!ip_current_is_v6()) {
        memset(out, 0, 16);
        return;
    }
    memcpy(out, ip6_current_dest_addr(), 16);
}

uint8_t a20_socket_bh_ip6_current_hoplimit(void)
{
    return ip_current_is_v6() ? IP6H_HOPLIM(ip6_current_header()) : 0;
}

uint8_t a20_socket_bh_ip6_current_tclass(void)
{
    return ip_current_is_v6() ? IP6H_TC(ip6_current_header()) : 0;
}

int a20_socket_bh_raw_should_passthrough_icmp_echo(const net_socket_t *s,
                                                   const struct pbuf *p)
{
    if (!s || !p)
        return 0;
    if (s->protocol != IPPROTO_ICMP || p->tot_len == 0)
        return 0;
    struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
    u16_t iphdr_hlen = IPH_HL_BYTES(iphdr);
    if (p->tot_len <= iphdr_hlen)
        return 0;
    uint8_t *payload = (uint8_t *)p->payload;
    return payload[iphdr_hlen] == ICMP_ECHO;
}
