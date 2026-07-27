#include "net/socket_internal.h"

#include "core/string.h"
#include "proc/proc.h"

#define NLMSG_DONE              3
#define NLM_F_MULTI             0x2
#define SOCK_DIAG_BY_FAMILY     20
#define TCPDIAG_GETSOCK         18

#define TCP_ESTABLISHED         1
#define TCP_CLOSE               7
#define TCP_LISTEN              10

typedef struct netlink_msghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
} netlink_msghdr_t;

typedef struct inet_diag_sockid {
    uint16_t idiag_sport;
    uint16_t idiag_dport;
    uint32_t idiag_src[4];
    uint32_t idiag_dst[4];
    uint32_t idiag_if;
    uint32_t idiag_cookie[2];
} inet_diag_sockid_t;

typedef struct inet_diag_req_v2 {
    uint8_t sdiag_family;
    uint8_t sdiag_protocol;
    uint8_t idiag_ext;
    uint8_t pad;
    uint32_t idiag_states;
    inet_diag_sockid_t id;
} inet_diag_req_v2_t;

typedef struct inet_diag_msg {
    uint8_t idiag_family;
    uint8_t idiag_state;
    uint8_t idiag_timer;
    uint8_t idiag_retrans;
    inet_diag_sockid_t id;
    uint32_t idiag_expires;
    uint32_t idiag_rqueue;
    uint32_t idiag_wqueue;
    uint32_t idiag_uid;
    uint32_t idiag_inode;
} inet_diag_msg_t;

typedef struct inet_diag_reply {
    netlink_msghdr_t nlh;
    inet_diag_msg_t diag;
} inet_diag_reply_t;

typedef struct netlink_done {
    netlink_msghdr_t nlh;
    int32_t error;
} netlink_done_t;

static uint8_t net_diag_state(const net_socket_t *s)
{
    if (s->listening)
        return TCP_LISTEN;
    if (s->connected)
        return TCP_ESTABLISHED;
    return TCP_CLOSE;
}

static int net_diag_protocol_matches(const net_socket_t *s, uint8_t protocol)
{
    if (protocol == IPPROTO_TCP)
        return s->type == SOCK_STREAM;
    if (protocol == IPPROTO_UDP)
        return s->type == SOCK_DGRAM;
    return s->type == SOCK_RAW && s->protocol == protocol;
}

static void net_diag_copy_endpoint(const uint8_t *addr, size_t addrlen,
                                   uint16_t *port, uint32_t out[4])
{
    if (!addr || addrlen < sizeof(uint16_t))
        return;
    uint16_t family = *(const uint16_t *)addr;
    if (family == AF_INET && addrlen >= sizeof(net_sockaddr_in_t)) {
        const net_sockaddr_in_t *in = (const net_sockaddr_in_t *)addr;
        *port = in->sin_port;
        out[0] = in->sin_addr;
    } else if (family == AF_INET6 && addrlen >= sizeof(net_sockaddr_in6_t)) {
        const net_sockaddr_in6_t *in6 = (const net_sockaddr_in6_t *)addr;
        *port = in6->sin6_port;
        memcpy(out, in6->sin6_addr, sizeof(in6->sin6_addr));
    }
}

static uint32_t net_diag_rx_bytes(const net_socket_t *s)
{
    uint64_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += m->len - m->off;
    return total > 0xffffffffULL ? 0xffffffffU : (uint32_t)total;
}

int net_netlink_bind(net_socket_t *s, const void *addr, size_t addrlen)
{
    if (!s || !addr || addrlen < sizeof(net_sockaddr_nl_t))
        return -EINVAL;
    const net_sockaddr_nl_t *requested = (const net_sockaddr_nl_t *)addr;
    if (requested->nl_family != AF_NETLINK)
        return -EAFNOSUPPORT;

    net_sockaddr_nl_t local = *requested;
    if (local.nl_pid == 0) {
        task_t *cur = proc_current();
        local.nl_pid = cur ? (uint32_t)cur->pid : 1;
    }
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    memcpy(s->local, &local, sizeof(local));
    s->local_len = sizeof(local);
    s->bound = 1;
    spin_unlock_irqrestore(&g_net_lock, irq);
    return 0;
}

int net_netlink_diag_request(net_socket_t *requester, const void *buf,
                             size_t len, const void *addr, size_t addrlen)
{
    if (!requester || requester->domain != AF_NETLINK ||
        requester->protocol != NETLINK_SOCK_DIAG)
        return -EPROTONOSUPPORT;
    if (!buf || len < sizeof(netlink_msghdr_t) + sizeof(inet_diag_req_v2_t))
        return -EINVAL;
    if (addr) {
        if (addrlen < sizeof(net_sockaddr_nl_t) ||
            ((const net_sockaddr_nl_t *)addr)->nl_family != AF_NETLINK)
            return -EAFNOSUPPORT;
    }

    const netlink_msghdr_t *req_nlh = (const netlink_msghdr_t *)buf;
    if (req_nlh->nlmsg_len < sizeof(netlink_msghdr_t) +
                              sizeof(inet_diag_req_v2_t) ||
        req_nlh->nlmsg_len > len ||
        (req_nlh->nlmsg_type != SOCK_DIAG_BY_FAMILY &&
         req_nlh->nlmsg_type != TCPDIAG_GETSOCK))
        return -EINVAL;
    const inet_diag_req_v2_t *req =
        (const inet_diag_req_v2_t *)((const uint8_t *)buf +
                                    sizeof(netlink_msghdr_t));

    net_sockaddr_nl_t kernel_addr = {
        .nl_family = AF_NETLINK,
        .nl_pid = 0,
        .nl_groups = 0,
    };
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    uint32_t recipient_pid = requester->local_len >= sizeof(net_sockaddr_nl_t)
        ? ((const net_sockaddr_nl_t *)requester->local)->nl_pid : 0;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || s == requester ||
            (s->domain != AF_INET && s->domain != AF_INET6) ||
            (req->sdiag_family != AF_UNSPEC &&
             s->domain != req->sdiag_family) ||
            !net_diag_protocol_matches(s, req->sdiag_protocol))
            continue;

        uint8_t state = net_diag_state(s);
        if (req->idiag_states && !(req->idiag_states & (1U << state)))
            continue;

        inet_diag_reply_t reply;
        memset(&reply, 0, sizeof(reply));
        reply.nlh.nlmsg_len = sizeof(reply);
        reply.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
        reply.nlh.nlmsg_flags = NLM_F_MULTI;
        reply.nlh.nlmsg_seq = req_nlh->nlmsg_seq;
        reply.nlh.nlmsg_pid = recipient_pid;
        reply.diag.idiag_family = (uint8_t)s->domain;
        reply.diag.idiag_state = state;
        net_diag_copy_endpoint(s->local, s->local_len,
                               &reply.diag.id.idiag_sport,
                               reply.diag.id.idiag_src);
        net_diag_copy_endpoint(s->peer_addr, s->peer_len,
                               &reply.diag.id.idiag_dport,
                               reply.diag.id.idiag_dst);
        reply.diag.id.idiag_cookie[0] = (uint32_t)(i + 1);
        reply.diag.id.idiag_cookie[1] = 0;
        reply.diag.idiag_rqueue = net_diag_rx_bytes(s);
        reply.diag.idiag_inode = (uint32_t)(i + 1);
        int r = net_enqueue_msg_locked(requester, &reply, sizeof(reply),
                                       &kernel_addr, sizeof(kernel_addr));
        if (r < 0) {
            spin_unlock_irqrestore(&g_net_lock, irq);
            return r;
        }
    }

    netlink_done_t done;
    memset(&done, 0, sizeof(done));
    done.nlh.nlmsg_len = sizeof(done);
    done.nlh.nlmsg_type = NLMSG_DONE;
    done.nlh.nlmsg_flags = NLM_F_MULTI;
    done.nlh.nlmsg_seq = req_nlh->nlmsg_seq;
    done.nlh.nlmsg_pid = recipient_pid;
    int r = net_enqueue_msg_locked(requester, &done, sizeof(done),
                                   &kernel_addr, sizeof(kernel_addr));
    spin_unlock_irqrestore(&g_net_lock, irq);
    return r < 0 ? r : (int)len;
}
