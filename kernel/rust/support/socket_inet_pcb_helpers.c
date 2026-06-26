#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "proc/proc.h"

#include "core/string.h"

#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/prot/icmp.h"

int a20_socket_inet_pcb_task_state(task_t *task)
{
    return task ? (int)task->state : 0;
}

uint64_t a20_socket_inet_pcb_ms_to_ticks(uint64_t ms)
{
    return MS_TO_TICKS(ms);
}

uint64_t a20_socket_inet_pcb_connect_timeout_ticks(void)
{
    return NET_CONNECT_TIMEOUT_TICKS;
}

static uint32_t bh_ring_mask(uint32_t idx)
{
    return idx & (NET_BH_RING_SIZE - 1);
}

static net_bh_event_t *bh_ring_prepare(net_bh_ring_t *r)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if ((head - tail) >= NET_BH_RING_SIZE)
        return NULL;
    net_bh_event_t *e = &r->events[bh_ring_mask(head)];
    memset(e, 0, sizeof(*e));
    e->type = NET_BH_RECV;
    return e;
}

static void bh_ring_commit(net_bh_ring_t *r)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_fetch_add(&r->head, 1, __ATOMIC_RELAXED);
}

static void net_inet_bh_schedule(net_socket_t *s)
{
    if (!s)
        return;
    int idx = s->reg_idx;
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return;
    __atomic_store_n(&s->bh_pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_net_bh_pending[idx], 1, __ATOMIC_RELEASE);
}

static void helper_fill_ipv6_meta(net_bh_event_t *e)
{
    if (!ip_current_is_v6())
        return;
    e->has_pktinfo = 1;
    e->pktinfo_ifindex = ip_current_input_netif() ?
        (uint32_t)netif_get_index(ip_current_input_netif()) : 0;
    memcpy(e->pktinfo_addr, ip6_current_dest_addr(), sizeof(e->pktinfo_addr));
    e->has_hoplimit = 1;
    e->hoplimit = IP6H_HOPLIM(ip6_current_header());
    e->has_tclass = 1;
    e->tclass = IP6H_TC(ip6_current_header());
}

#ifdef CONFIG_RUST_SOCKET_INET_BH
extern void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port);
extern u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr);
extern err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err);
extern err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                              err_t err);
extern void lwip_tcp_err_cb(void *arg, err_t err);
extern err_t lwip_tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len);
#else
static void lwip_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return;
    net_bh_event_t *e = bh_ring_prepare(&s->bh_ring);
    if (!e) {
        pbuf_free(p);
        return;
    }
    net_lwip_ip_to_sockaddr(addr, port, e->addr, &e->addrlen);
    helper_fill_ipv6_meta(e);
    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    pbuf_copy_partial(p, e->data, (u16_t)len, 0);
    e->len = len;
    bh_ring_commit(&s->bh_ring);
    net_inet_bh_schedule(s);
    pbuf_free(p);
}

static u8_t lwip_raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s || !p)
        return 0;
    if (s->protocol == IPPROTO_ICMP && p->tot_len > 0) {
        struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
        u16_t iphdr_hlen = IPH_HL_BYTES(iphdr);
        if (p->tot_len > iphdr_hlen) {
            uint8_t *payload = (uint8_t *)p->payload;
            if (payload[iphdr_hlen] == ICMP_ECHO)
                return 0;
        }
    }
    net_bh_event_t *e = bh_ring_prepare(&s->bh_ring);
    if (!e) {
        pbuf_free(p);
        return 0;
    }
    net_lwip_ip_to_sockaddr(addr, 0, e->addr, &e->addrlen);
    helper_fill_ipv6_meta(e);
    size_t len = p->tot_len;
    if (len > NET_MAX_PAYLOAD)
        len = NET_MAX_PAYLOAD;
    pbuf_copy_partial(p, e->data, (u16_t)len, 0);
    e->len = len;
    bh_ring_commit(&s->bh_ring);
    net_inet_bh_schedule(s);
    pbuf_free(p);
    return 1;
}

static err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    __atomic_store_n(&s->bh_err_code, (int)err, __ATOMIC_RELEASE);
    __atomic_store_n(&s->bh_connected, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
    return ERR_OK;
}

static err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                              err_t err)
{
    (void)pcb;
    (void)err;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    if (!p) {
        __atomic_store_n(&s->bh_closed, 1, __ATOMIC_RELEASE);
        net_inet_bh_schedule(s);
        return ERR_OK;
    }
    size_t off = 0;
    while (off < p->tot_len) {
        net_bh_event_t *e = bh_ring_prepare(&s->bh_ring);
        if (!e) {
            pbuf_free(p);
            return ERR_MEM;
        }
        size_t n = p->tot_len - off;
        if (n > NET_MAX_PAYLOAD)
            n = NET_MAX_PAYLOAD;
        pbuf_copy_partial(p, e->data, (u16_t)n, (u16_t)off);
        e->len = n;
        bh_ring_commit(&s->bh_ring);
        off += n;
    }
    net_inet_bh_schedule(s);
    pbuf_free(p);
    return ERR_OK;
}

static void lwip_tcp_err_cb(void *arg, err_t err)
{
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return;
    s->tcp = NULL;
    __atomic_store_n(&s->bh_err_code, (int)err, __ATOMIC_RELEASE);
    __atomic_store_n(&s->bh_error, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
}

static err_t lwip_tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb;
    (void)len;
    net_socket_t *s = (net_socket_t *)arg;
    if (!s)
        return ERR_OK;
    __atomic_store_n(&s->bh_tx_wake, 1, __ATOMIC_RELEASE);
    net_inet_bh_schedule(s);
    return ERR_OK;
}
#endif

struct udp_pcb *a20_socket_inet_udp_new(int domain, net_socket_t *s)
{
    uint64_t flags = a20_lwip_lock();
    struct udp_pcb *pcb = udp_new_ip_type(domain == AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
    if (pcb)
        udp_recv(pcb, lwip_udp_recv_cb, s);
    a20_lwip_unlock(flags);
    return pcb;
}

struct raw_pcb *a20_socket_inet_raw_new(int domain, int protocol, net_socket_t *s)
{
    uint64_t flags = a20_lwip_lock();
    struct raw_pcb *pcb = raw_new_ip_type(domain == AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4,
                                          (u8_t)protocol);
    if (pcb)
        raw_recv(pcb, lwip_raw_recv_cb, s);
    a20_lwip_unlock(flags);
    return pcb;
}

struct tcp_pcb *a20_socket_inet_tcp_new_v4(net_socket_t *s)
{
    uint64_t flags = a20_lwip_lock();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb) {
        if (s->tcp_nodelay)
            tcp_nagle_disable(pcb);
        if (s->keepalive)
            pcb->so_options |= SOF_KEEPALIVE;
        if (s->keep_idle > 0)
            pcb->keep_idle = (u32_t)s->keep_idle * 1000U;
        if (s->keep_intvl > 0)
            pcb->keep_intvl = (u32_t)s->keep_intvl * 1000U;
        if (s->keep_cnt > 0)
            pcb->keep_cnt = (u32_t)s->keep_cnt;
        tcp_arg(pcb, s);
        tcp_recv(pcb, lwip_tcp_recv_cb);
        tcp_err(pcb, lwip_tcp_err_cb);
        tcp_sent(pcb, lwip_tcp_sent_cb);
    }
    a20_lwip_unlock(flags);
    return pcb;
}

void a20_socket_inet_udp_remove(struct udp_pcb *pcb)
{
    uint64_t flags = a20_lwip_lock();
    udp_remove(pcb);
    a20_lwip_unlock(flags);
}

void a20_socket_inet_raw_remove(struct raw_pcb *pcb)
{
    uint64_t flags = a20_lwip_lock();
    raw_remove(pcb);
    a20_lwip_unlock(flags);
}

void a20_socket_inet_tcp_destroy_abort(struct tcp_pcb *pcb)
{
    uint64_t flags = a20_lwip_lock();
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_abort(pcb);
    a20_lwip_unlock(flags);
}

int a20_socket_inet_udp_bind(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = udp_bind(pcb, ip, port);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_udp_bind_any(struct udp_pcb *pcb, u16_t port)
{
    ip_addr_t any;
    ip_addr_set_zero_ip4(&any);
    return a20_socket_inet_udp_bind(pcb, &any, port);
}

int a20_socket_inet_raw_bind(struct raw_pcb *pcb, const ip_addr_t *ip)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = raw_bind(pcb, ip);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_tcp_bind(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = tcp_bind(pcb, ip, port);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_udp_connect(struct udp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = udp_connect(pcb, ip, port);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_raw_connect(struct raw_pcb *pcb, const ip_addr_t *ip)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = raw_connect(pcb, ip);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_tcp_connect(struct tcp_pcb *pcb, const ip_addr_t *ip, u16_t port)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = tcp_connect(pcb, ip, port, lwip_tcp_connected_cb);
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_udp_sendto(struct udp_pcb *pcb, const void *buf, size_t len,
                               const ip_addr_t *ip, u16_t port, int connected)
{
    uint64_t flags = a20_lwip_lock();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) {
        a20_lwip_unlock(flags);
        return ERR_MEM;
    }
    pbuf_take(p, buf, (u16_t)len);
    err_t e = connected ? udp_send(pcb, p) : udp_sendto(pcb, p, ip, port);
    pbuf_free(p);
    a20_lwip_poll_locked();
    a20_lwip_unlock(flags);
    return (int)e;
}

int a20_socket_inet_raw_sendto(struct raw_pcb *pcb, const void *buf, size_t len,
                               const ip_addr_t *ip, int connected)
{
    uint64_t flags = a20_lwip_lock();
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) {
        a20_lwip_unlock(flags);
        return ERR_MEM;
    }
    pbuf_take(p, buf, (u16_t)len);
    err_t e = connected ? raw_send(pcb, p) : raw_sendto(pcb, p, ip);
    pbuf_free(p);
    a20_lwip_poll_locked();
    a20_lwip_unlock(flags);
    return (int)e;
}

void a20_socket_inet_tcp_close_socket(net_socket_t *s)
{
    uint64_t flags = a20_lwip_lock();
    tcp_arg(s->tcp, NULL);
    if (s->listening) {
        tcp_accept(s->tcp, NULL);
    } else {
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_sent(s->tcp, NULL);
    }
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
    a20_lwip_unlock(flags);
}

void a20_socket_inet_tcp_drop_socket(net_socket_t *s)
{
    uint64_t flags = a20_lwip_lock();
    tcp_arg(s->tcp, NULL);
    tcp_recv(s->tcp, NULL);
    tcp_err(s->tcp, NULL);
    tcp_sent(s->tcp, NULL);
    if (tcp_close(s->tcp) != ERR_OK)
        tcp_abort(s->tcp);
    s->tcp = NULL;
    a20_lwip_unlock(flags);
}

u16_t a20_socket_inet_tcp_sndbuf(struct tcp_pcb *pcb)
{
    uint64_t flags = a20_lwip_lock();
    u16_t room = tcp_sndbuf(pcb);
    a20_lwip_unlock(flags);
    return room;
}

int a20_socket_inet_tcp_write_output(struct tcp_pcb *pcb, const void *buf, u16_t len)
{
    uint64_t flags = a20_lwip_lock();
    err_t e = tcp_write(pcb, buf, len, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK)
        e = tcp_output(pcb);
    a20_lwip_unlock(flags);
    return (int)e;
}

void a20_socket_inet_tcp_backlog_accepted(struct tcp_pcb *pcb)
{
    uint64_t flags = a20_lwip_lock();
    tcp_backlog_accepted(pcb);
    a20_lwip_unlock(flags);
}

void a20_socket_inet_tcp_recved(struct tcp_pcb *pcb, size_t len)
{
    uint64_t flags = a20_lwip_lock();
    while (len > 0) {
        uint16_t n = len > 0xFFFF ? 0xFFFF : (uint16_t)len;
        tcp_recved(pcb, n);
        len -= n;
    }
    a20_lwip_unlock(flags);
}
