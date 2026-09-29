#include "net/socket_internal.h"
#include "core/consts.h"

/*
 * Socket-table enumeration behind /proc/net/{tcp,udp,unix}.
 *
 * Everything in this file runs under g_net_lock and nothing else.  procfs
 * renders its rows from the walk callback, so that callback executes in
 * spinlock context: no blocking, no allocation, and no lwIP call, because
 * g_lwip_lock and g_net_lock are never held together
 * (docs/net/network-lock-contract.md).
 */

/*
 * Membership is decided by domain and type, not by "does the socket still own
 * a pcb".  A loopback connect() pairs two kernel-internal sockets and then
 * drops both lwIP pcbs (net_inet_connect_stream -> net_tcp_drop_pcb), so a
 * pcb test would hide exactly the connections a loopback-only stack makes.
 * `closed` is the teardown signal: net_socket_close_file sets it in the same
 * g_net_lock section that unregisters the slot, so a table snapshot can never
 * catch a half-freed socket.
 */
static int net_table_claims(net_table_kind_t kind, const net_socket_t *s)
{
    switch (kind) {
    case NET_TABLE_TCP:
        return (s->domain == AF_INET || s->domain == AF_INET6) &&
               s->type == SOCK_STREAM;
    case NET_TABLE_UDP:
        /* net_inet_socket_init gives every AF_INET/AF_INET6 datagram socket
         * either a udp or a raw pcb and fails the socket() otherwise, so the
         * pcb test is also how a live socket is told from a torn-down one. */
        return (s->domain == AF_INET || s->domain == AF_INET6) &&
               (s->udp || s->raw);
    case NET_TABLE_UNIX:
        return s->domain == AF_UNIX;
    }
    return 0;
}

/* Visits every socket of `kind` still owned by the table.  Returns the number
 * of visits, or -EINVAL when there is no callback to run. */
int net_socket_table_walk(net_table_kind_t kind, net_table_visit_fn fn,
                          void *arg)
{
    if (!fn)
        return -EINVAL;

    int visited = 0;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        net_socket_t *s = g_sockets[i];
        if (!s || s->closed || !net_table_claims(kind, s))
            continue;
        visited++;
        fn(s, arg);
    }
    spin_unlock_irqrestore(&g_net_lock, irq);
    return visited;
}

/*
 * Bytes a recv() would hand over right now.  A stream socket may have a partly
 * consumed head message, so only len-off counts on it; a datagram or
 * seqpacket socket is always handed over whole.
 *
 * This is the whole readable set except for the channel-backed AF_UNIX data
 * plane, where plain bytes sit in an a20_channel and net_socket_t tracks no
 * pending count for them (ch_len is the staging buffer peek()/recv() fill on
 * demand, not a backlog, so counting it would report a number that has
 * nothing to do with what is readable).
 */
static size_t net_rx_queued_locked(const net_socket_t *s)
{
    size_t total = 0;
    for (const net_msg_t *m = s->rx_head; m; m = m->next)
        total += (s->type == SOCK_STREAM) ? (m->len - m->off) : m->len;
    return total;
}

int net_socket_rx_available(net_socket_t *s, size_t *out)
{
    if (!out)
        return -EINVAL;
    if (!s)
        return -ENOTSOCK;

    size_t total;
    uint64_t irq = spin_lock_irqsave(&g_net_lock);
    if (!net_socket_is_valid_locked(s)) {
        spin_unlock_irqrestore(&g_net_lock, irq);
        return -ENOTSOCK;
    }
    if (s->domain == AF_PACKET) {
        /* Captured frames are whole L2 payloads with no stream position, so
         * there is no byte count this socket can honestly report. */
        spin_unlock_irqrestore(&g_net_lock, irq);
        return -EOPNOTSUPP;
    }
    total = net_rx_queued_locked(s);
    spin_unlock_irqrestore(&g_net_lock, irq);
    *out = total;
    return 0;
}
