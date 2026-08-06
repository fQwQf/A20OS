/*
 * netd socket service: lwIP TCP/UDP connection management behind the
 * kernel socket proxy (netd_sock_proto.h).  One RPC channel per socket;
 * blocking ops (accept/connect/recv) are held until lwIP callbacks make
 * them satisfiable.
 */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/timeouts.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/a20_mem.h"
#include "liba20rt/a20_event.h"
#include "liba20rt/a20_syscall.h"
#include "liba20c/include/stdarg.h"

#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_NONBLOCK 0x800
#endif
#ifndef AF_INET
#define AF_INET 2
#define AF_INET6 10
#endif
#ifndef EADDRINUSE
#define EADDRINUSE 98
#define EAGAIN 11
#define ECONNRESET 104
#define ECONNREFUSED 111
#define EIO 5
#define ENOTCONN 107
#define EINVAL 22
#define ENOMEM 12
#define EBADF 9
#define EAFNOSUPPORT 97
#define EISCONN 106
#define EMSGSIZE 90
#define EINPROGRESS 115
#endif
#include "../net/lwip/netd_sock_proto.h"

#define NETD_SOCK_POOL 64
#define NETD_SOCK_MAX_ACCEPT 64

typedef struct netd_sock {
    int used;
    int type;                  /* SOCK_STREAM / SOCK_DGRAM */
    a20_handle_t ep;           /* RPC channel endpoint (NULL for children) */
    a20_handle_t rpc_ep;       /* channel used for replies (children: listener's) */
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    int listening;
    int connected;
    int closed;
    int err;                   /* lwIP error code */
    /* TCP receive buffer */
    uint8_t  rbuf[NETD_SOCK_MAX_DATA];
    uint32_t rlen;
    /* pending ops */
    int recv_pending;          /* a RECV request is waiting */
    uint32_t recv_want;
    int accept_pending;        /* an ACCEPT request is waiting */
    /* accepted-connection queue (listener) */
    struct netd_sock *aq[NETD_SOCK_MAX_ACCEPT];
    int aq_n;
    int aq_head;
    /* peer address (accepted/connected) */
    netd_sock_addr_t peer;
} netd_sock_t;

static netd_sock_t g_socks[NETD_SOCK_POOL];
static a20_handle_t g_eventq = A20_HANDLE_NULL;
static int g_netd_sock_ready;

extern void a20_netd_printf(const char *fmt, ...);
extern void netd_rx_drain(void);

static netd_sock_t *sock_by_id(int id)
{
    if (id < 0 || id >= NETD_SOCK_POOL || !g_socks[id].used)
        return NULL;
    return &g_socks[id];
}

static int sock_alloc(void)
{
    for (int i = 0; i < NETD_SOCK_POOL; i++) {
        if (!g_socks[i].used && g_socks[i].ep == A20_HANDLE_NULL) {
            g_socks[i].used = 1;
            g_socks[i].type = 0;
            g_socks[i].tcp = NULL;
            g_socks[i].udp = NULL;
            g_socks[i].listening = 0;
            g_socks[i].connected = 0;
            g_socks[i].closed = 0;
            g_socks[i].err = 0;
            g_socks[i].rlen = 0;
            g_socks[i].recv_pending = 0;
            g_socks[i].accept_pending = 0;
            g_socks[i].aq_n = 0;
            g_socks[i].aq_head = 0;
            g_socks[i].rpc_ep = A20_HANDLE_NULL;
            return i;
        }
    }
    return -1;
}

static void send_reply(netd_sock_t *s, const netd_sock_hdr_t *h,
                       const void *payload, uint32_t paylen)
{
    uint8_t msg[NETD_SOCK_MAX_DATA + 64];
    if (sizeof(*h) + paylen > sizeof(msg))
        paylen = sizeof(msg) - sizeof(*h);
    netd_sock_hdr_t *rh = (netd_sock_hdr_t *)msg;
    *rh = *h;
    rh->len = paylen;
    if (paylen)
        __builtin_memcpy(msg + sizeof(*h), payload, paylen);
    a20_channel_send(s->rpc_ep ? s->rpc_ep : s->ep, msg,
                     sizeof(*h) + paylen, 0, 0);
}

/* ---- TCP callbacks ---- */

static err_t tcp_cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err);

static void tcp_cb_err(void *arg, err_t err)
{
    netd_sock_t *s = (netd_sock_t *)arg;
    if (!s) return;
    s->err = (int)err;
    if (err == ERR_ABRT && s->tcp) {
        s->tcp = NULL;
        s->connected = 0;
    }
    if (s->recv_pending) {
        s->recv_pending = 0;
        netd_sock_hdr_t h;
        __builtin_memset(&h, 0, sizeof(h));
        h.op = NETD_SOCK_OP_RECV;
        h.id = (int32_t)(s - g_socks);
        h.status = -ECONNRESET;
        send_reply(s, &h, NULL, 0);
    }
}

static err_t tcp_cb_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    netd_sock_t *ls = (netd_sock_t *)arg;

    if (!ls || err != ERR_OK)
        return ERR_ABRT;
    /* lwIP invokes the accept callback while the connection is still in
     * SYN_RCVD (tcp_process), using the listener's callback arg; guard
     * against re-entry through a non-listening socket. */
    if (!ls->listening || ls->closed)
        return ERR_OK;
    int id = sock_alloc();
    if (id < 0)
        return ERR_MEM;
    netd_sock_t *s = &g_socks[id];
    s->type = SOCK_STREAM;
    s->ep = A20_HANDLE_NULL; /* child RPC rides the listener's channel (h->id) */
    s->rpc_ep = ls->ep;      /* replies go out on the listener's channel */
    s->tcp = newpcb;
    s->connected = 1;
    tcp_arg(newpcb, s);
    tcp_recv(newpcb, tcp_cb_recv);
    tcp_err(newpcb, tcp_cb_err);
    /* record peer address */
    __builtin_memset(&s->peer, 0, sizeof(s->peer));
    s->peer.family = AF_INET;
    s->peer.port = newpcb->remote_port;
    ip4_addr_t *rip4 = ip_2_ip4(&newpcb->remote_ip);
    __builtin_memcpy(s->peer.addr, rip4, 4);

    if (ls->accept_pending) {
        /* hand the connection straight to the blocked ACCEPT */
        ls->accept_pending = 0;
        netd_sock_hdr_t h;
        __builtin_memset(&h, 0, sizeof(h));
        h.op = NETD_SOCK_OP_ACCEPT;
        h.id = (int32_t)id;
        send_reply(ls, &h, &s->peer, sizeof(s->peer));
    } else if (ls->aq_n < NETD_SOCK_MAX_ACCEPT) {
        ls->aq[(ls->aq_head + ls->aq_n) % NETD_SOCK_MAX_ACCEPT] = s;
        ls->aq_n++;
    }
    return ERR_OK;
}

static err_t tcp_cb_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err)
{
    netd_sock_t *s = (netd_sock_t *)arg;
    if (!s) return ERR_ABRT;
    if (p)
        if (p == NULL) {
        /* peer closed */
        s->connected = 0;
        if (s->recv_pending) {
            s->recv_pending = 0;
            netd_sock_hdr_t h;
            __builtin_memset(&h, 0, sizeof(h));
            h.op = NETD_SOCK_OP_RECV;
            h.id = (int32_t)(s - g_socks);
            h.status = 0;
            h.len = 0;
            send_reply(s, &h, NULL, 0);
        }
        return ERR_OK;
    }
    while (p) {
        uint32_t n = p->len;
        if (s->rlen + n > NETD_SOCK_MAX_DATA)
            n = NETD_SOCK_MAX_DATA - s->rlen;
        __builtin_memcpy(s->rbuf + s->rlen, p->payload, n);
        s->rlen += n;
        p = p->next;
    }
    tcp_recved(pcb, p ? 0 : 1);
    if (s->recv_pending && s->rlen > 0) {
        s->recv_pending = 0;
        uint32_t take = s->rlen < s->recv_want ? s->rlen : s->recv_want;
        netd_sock_recv_resp_t *resp = (netd_sock_recv_resp_t *)__builtin_alloca(
            sizeof(netd_sock_recv_resp_t) + take);
        resp->data_len = take;
        __builtin_memset(&resp->addr, 0, sizeof(resp->addr));
        __builtin_memcpy(resp->data, s->rbuf, take);
        __builtin_memmove(s->rbuf, s->rbuf + take, s->rlen - take);
        s->rlen -= take;
        netd_sock_hdr_t h;
        __builtin_memset(&h, 0, sizeof(h));
        h.op = NETD_SOCK_OP_RECV;
        h.id = (int32_t)(s - g_socks);
        send_reply(s, &h, resp, sizeof(*resp) + take);
    }
    return ERR_OK;
}

static err_t tcp_cb_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    netd_sock_t *s = (netd_sock_t *)arg;
    if (!s) return ERR_ABRT;
    if (err == ERR_OK) {
        s->connected = 1;
    } else {
        s->err = (int)err;
    }
    s->tcp = pcb;
    tcp_recv(pcb, tcp_cb_recv);
    tcp_err(pcb, tcp_cb_err);
}

/* ---- operation handlers ---- */

static int op_create(netd_sock_t *s, const netd_sock_hdr_t *h,
                     const uint8_t *payload)
{

    const netd_sock_create_req_t *req = (const netd_sock_create_req_t *)payload;
    s->type = req->type & 0xf;
    s->used = 1;
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_CREATE;
    resp.id = (int32_t)(s - g_socks);
    resp.status = 0;
    send_reply(s, &resp, &resp.id, sizeof(resp.id));
    return 0;
}

static int op_bind(netd_sock_t *s, const netd_sock_hdr_t *h,
                   const uint8_t *payload)
{
    const netd_sock_addr_t *a = (const netd_sock_addr_t *)payload;
    err_t er = ERR_OK;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp) s->tcp = tcp_new();
        if (!s->tcp) er = ERR_MEM;
        else {
            ip_addr_t ip;
            ip4_addr_t ip4;
            IP4_ADDR(&ip4, a->addr[0], a->addr[1], a->addr[2], a->addr[3]);
            ip_addr_copy_from_ip4(ip, ip4);
            er = tcp_bind(s->tcp, &ip, lwip_htons(a->port));
        }
    } else if (s->type == SOCK_DGRAM) {
        if (!s->udp) s->udp = udp_new();
        if (!s->udp) er = ERR_MEM;
        else {
            ip_addr_t ip;
            ip4_addr_t ip4;
            IP4_ADDR(&ip4, a->addr[0], a->addr[1], a->addr[2], a->addr[3]);
            ip_addr_copy_from_ip4(ip, ip4);
            er = udp_bind(s->udp, &ip, lwip_htons(a->port));
        }
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_BIND;
    resp.id = h->id;
    resp.status = er == ERR_OK ? 0 : -EADDRINUSE;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_listen(netd_sock_t *s, const netd_sock_hdr_t *h,
                     const uint8_t *payload)
{
    (void)payload;
    err_t er = ERR_OK;

    if (s->tcp) {
        tcp_arg(s->tcp, s);
        struct tcp_pcb *l = tcp_listen_with_backlog(s->tcp, 8);
        if (!l) er = ERR_MEM;
        else {
            s->tcp = l;
            s->listening = 1;
            tcp_accept(l, tcp_cb_accept);
        }
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_LISTEN;
    resp.id = h->id;
    resp.status = er == ERR_OK ? 0 : -EADDRINUSE;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_connect(netd_sock_t *s, const netd_sock_hdr_t *h,
                      const uint8_t *payload, uint32_t paylen)
{
    (void)paylen;
    const netd_sock_addr_t *a = (const netd_sock_addr_t *)payload;
    err_t er;

    if (s->type == SOCK_STREAM) {
        if (!s->tcp) s->tcp = tcp_new();
        if (!s->tcp) er = ERR_MEM;
        else {
            ip_addr_t ip;
            ip4_addr_t ip4;
            IP4_ADDR(&ip4, a->addr[0], a->addr[1], a->addr[2], a->addr[3]);
            ip_addr_copy_from_ip4(ip, ip4);
            tcp_arg(s->tcp, s);
            tcp_err(s->tcp, tcp_cb_err);
            er = tcp_connect(s->tcp, &ip, lwip_htons(a->port), tcp_cb_connected);
        }
    } else {
        /* UDP "connect": record the default peer. */
        if (!s->udp) s->udp = udp_new();
        __builtin_memcpy(&s->peer, a, sizeof(*a));
        s->connected = 1;
        er = ERR_OK;
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_CONNECT;
    resp.id = h->id;
    resp.status = er == ERR_OK ? 0 : -ECONNREFUSED;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_accept(netd_sock_t *s, const netd_sock_hdr_t *h,
                     const uint8_t *payload)
{
    uint32_t nonblock = payload ? *(const uint32_t *)payload : 0;

    if (s->aq_n > 0) {
        netd_sock_t *c = s->aq[s->aq_head];
        s->aq_head = (s->aq_head + 1) % NETD_SOCK_MAX_ACCEPT;
        s->aq_n--;
        netd_sock_hdr_t resp;
        __builtin_memset(&resp, 0, sizeof(resp));
        resp.op = NETD_SOCK_OP_ACCEPT;
        resp.id = (int32_t)(c - g_socks);
        send_reply(s, &resp, &c->peer, sizeof(c->peer));
        return 0;
    }
    if (nonblock) {
        netd_sock_hdr_t resp;
        __builtin_memset(&resp, 0, sizeof(resp));
        resp.op = NETD_SOCK_OP_ACCEPT;
        resp.id = h->id;
        resp.status = -EAGAIN;
        send_reply(s, &resp, NULL, 0);
        return 0;
    }
    s->accept_pending = 1; /* reply when tcp_cb_accept fires */
    return 0;
}

static int op_send(netd_sock_t *s, const netd_sock_hdr_t *h,
                   const uint8_t *payload, uint32_t paylen)
{
    const netd_sock_send_req_t *req = (const netd_sock_send_req_t *)payload;
    uint32_t n = req->data_len;
    err_t er = ERR_OK;
    if (s->type == SOCK_STREAM) {
        if (!s->tcp || !s->connected) {
            er = ERR_CONN;
        } else {
            err_t wer = tcp_write(s->tcp, req->data, n, TCP_WRITE_FLAG_COPY);
            if (wer == ERR_OK)
                tcp_output(s->tcp);
            er = wer;
        }
    } else if (s->type == SOCK_DGRAM) {
        if (s->udp) {
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, n, PBUF_RAM);
            if (!p) er = ERR_MEM;
            else {
                __builtin_memcpy(p->payload, req->data, n);
                ip_addr_t ip;
                ip4_addr_t ip4;
                IP4_ADDR(&ip4, req->addr.addr[0], req->addr.addr[1],
                         req->addr.addr[2], req->addr.addr[3]);
                ip_addr_copy_from_ip4(ip, ip4);
                er = udp_sendto(s->udp, p, &ip, lwip_htons(req->addr.port));
                pbuf_free(p);
            }
        } else er = ERR_CONN;
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_SEND;
    resp.id = h->id;
    resp.status = er == ERR_OK ? 0 : -EIO;
    resp.len = n;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_recv(netd_sock_t *s, const netd_sock_hdr_t *h,
                   const uint8_t *payload)
{
    uint32_t want = payload ? *(const uint32_t *)payload : NETD_SOCK_MAX_DATA;
    if (s->type == SOCK_STREAM) {
        if (s->rlen > 0) {
            uint32_t take = s->rlen < want ? s->rlen : want;
            netd_sock_recv_resp_t *resp = (netd_sock_recv_resp_t *)__builtin_alloca(
                sizeof(netd_sock_recv_resp_t) + take);
            resp->data_len = take;
            __builtin_memset(&resp->addr, 0, sizeof(resp->addr));
            __builtin_memcpy(resp->data, s->rbuf, take);
            __builtin_memmove(s->rbuf, s->rbuf + take, s->rlen - take);
            s->rlen -= take;
            netd_sock_hdr_t resp2;
            __builtin_memset(&resp2, 0, sizeof(resp2));
            resp2.op = NETD_SOCK_OP_RECV;
            resp2.id = h->id;
            send_reply(s, &resp2, resp, sizeof(*resp) + take);
            return 0;
        }
        if (!s->connected && !s->err) {
            /* listener with pending data? not a stream data source */
        }
        s->recv_pending = 1;
        s->recv_want = want;
        return 0; /* reply when data arrives or peer closes */
    }
    /* UDP: no queued datagram -> EAGAIN (nonblocking userspace model). */
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_RECV;
    resp.id = h->id;
    resp.status = -EAGAIN;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_close(netd_sock_t *s, const netd_sock_hdr_t *h)
{
    if (s->tcp) {
        tcp_arg(s->tcp, NULL);
        tcp_close(s->tcp);
        s->tcp = NULL;
    }
    if (s->udp) {
        udp_remove(s->udp);
        s->udp = NULL;
    }
    s->closed = 1;
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_CLOSE;
    resp.id = h->id;
    send_reply(s, &resp, NULL, 0);
    s->used = 0;
    return 0;
}

static int op_poll(netd_sock_t *s, const netd_sock_hdr_t *h,
                   const uint8_t *payload)
{
    uint32_t events = payload ? *(const uint32_t *)payload : 0;
    uint32_t ready = 0;
    if (events & 1u) { /* POLLIN */
        if (s->type == SOCK_STREAM && (s->rlen > 0 || !s->connected))
            ready |= 1u;
        if (s->listening && s->aq_n > 0)
            ready |= 1u;
    }
    if (events & 2u) { /* POLLOUT */
        if (s->type == SOCK_STREAM ? (s->connected || s->listening) : 1)
            ready |= 2u;
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_POLL;
    resp.id = h->id;
    resp.flags = ready;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

static int op_getsockname(netd_sock_t *s, const netd_sock_hdr_t *h)
{
    netd_sock_addr_t a;
    __builtin_memset(&a, 0, sizeof(a));
    if (s->tcp) {
        a.family = AF_INET;
        a.port = lwip_htons(s->tcp->local_port);
        ip4_addr_t *lip4 = ip_2_ip4(&s->tcp->local_ip);
        __builtin_memcpy(a.addr, lip4, 4);
    }
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_GETSOCKNAME;
    resp.id = h->id;
    send_reply(s, &resp, &a, sizeof(a));
    return 0;
}

static int op_getpeername(netd_sock_t *s, const netd_sock_hdr_t *h)
{
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_GETPEERNAME;
    resp.id = h->id;
    resp.status = s->connected ? 0 : -ENOTCONN;
    send_reply(s, &resp, &s->peer, sizeof(s->peer));
    return 0;
}

static int op_setsockopt(netd_sock_t *s, const netd_sock_hdr_t *h,
                         const uint8_t *payload, uint32_t paylen)
{
    (void)s; (void)h; (void)payload; (void)paylen;
    netd_sock_hdr_t resp;
    __builtin_memset(&resp, 0, sizeof(resp));
    resp.op = NETD_SOCK_OP_SETSOCKOPT;
    resp.id = h->id;
    send_reply(s, &resp, NULL, 0);
    return 0;
}

/* ---- service loop ---- */

void netd_sock_run(void)
{
    extern uint32_t sys_now(void);
    (void)sys_now;
    g_netd_sock_ready = 1;
    for (;;) {
        /* Drain ready RPC channels. */
        for (int i = 0; i < NETD_SOCK_POOL; i++) {
            netd_sock_t *s = &g_socks[i];
            if (s->ep == A20_HANDLE_NULL || s->closed)
                continue;
            uint32_t dlen = NETD_SOCK_MAX_DATA + 64;
            uint8_t msg[NETD_SOCK_MAX_DATA + 64];
            uint32_t hcnt = 0;
            a20_status_t r = a20_channel_recv_flags(s->ep, msg, &dlen, 0,
                                              &hcnt, A20_MSG_NONBLOCK);
            if (r == -A20_ERR_WOULD_BLOCK)
                continue;
            if (r < 0)
                continue;
            const netd_sock_hdr_t *h = (const netd_sock_hdr_t *)msg;
            uint32_t pay = dlen > sizeof(*h) ? dlen - sizeof(*h) : 0;
            const uint8_t *pl = pay ? msg + sizeof(*h) : NULL;
            /* Requests identify their socket by h->id (accepted children
             * share the listener's channel); only CREATE uses the polled
             * slot (its id is -1). */
            if (h->op == NETD_SOCK_OP_CREATE) {
                op_create(s, h, pl);
                continue;
            }
            netd_sock_t *target = sock_by_id(h->id);
            if (!target)
                continue;
            switch (h->op) {
            case NETD_SOCK_OP_BIND: op_bind(target, h, pl); break;
            case NETD_SOCK_OP_LISTEN: op_listen(target, h, pl); break;
            case NETD_SOCK_OP_CONNECT: op_connect(target, h, pl, pay); break;
            case NETD_SOCK_OP_ACCEPT: op_accept(target, h, pl); break;
            case NETD_SOCK_OP_SEND: op_send(target, h, pl, pay); break;
            case NETD_SOCK_OP_RECV: op_recv(target, h, pl); break;
            case NETD_SOCK_OP_CLOSE: op_close(target, h); break;
            case NETD_SOCK_OP_GETSOCKNAME: op_getsockname(target, h); break;
            case NETD_SOCK_OP_GETPEERNAME: op_getpeername(target, h); break;
            case NETD_SOCK_OP_SETSOCKOPT: op_setsockopt(target, h, pl, pay); break;
            case NETD_SOCK_OP_POLL: op_poll(target, h, pl); break;
            default: break;
            }
        }
        netd_rx_drain();
        sys_check_timeouts();
        netif_poll(netif_default);

        a20_thread_yield();
    }
}

/* Register RPC channels; called once at netd startup. */
int netd_sock_init(void)
{
    int n = 0;
    /* Register only half the pool as RPC channel slots; the remaining
     * slots (ep == A20_HANDLE_NULL) back accepted child sockets. */
    for (int i = 0; i < NETD_SOCK_POOL; i++) {
        g_socks[i].used = 0;
        g_socks[i].ep = A20_HANDLE_NULL;
        g_socks[i].rpc_ep = A20_HANDLE_NULL;
        g_socks[i].type = 0;
    }
    for (int i = 0; i < NETD_SOCK_POOL / 2; i++) {

        int64_t h = a20_syscall6(A20_SYS_netd_sock_register, 0, 0, 0, 0, 0, 0);
        if (h < 0) {

            break;
        }
        g_socks[i].used = 0;
        g_socks[i].ep = (a20_handle_t)h;
        g_socks[i].rpc_ep = (a20_handle_t)h;
        g_socks[i].type = 0;
        n++;
    }

    return 0;
}
