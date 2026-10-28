#include "net/socket/socket_internal.h"
#include "net/socket_side.h"
#include "core/consts.h"

/*
 * Socket-table enumeration behind /proc/net/{tcp,udp,unix}.
 *
 * This file runs under the socket's own lock, nested inside a bucket lock
 * that is held only to reach the slot table.  procfs
 * renders its rows from the walk callback, so that callback executes in
 * spinlock context: no blocking, no allocation, and no lwIP call, because
 * g_lwip_lock and a net lock are never held together
 * (docs/net/network-lock-contract.md).
 *
 * The walk takes ONE bucket lock at a time and releases it before taking the
 * next.  Taking all of them would be the livelock fs/vfs/dcache.c records --
 * with interrupts disabled, an interrupt that reaches a lookup then spins on a
 * lock its own interrupted context was holding -- and vfs_dcache_invalidate_all
 * is the shape used there.
 */

/*
 * Membership is decided by domain and type, not by "does the socket still own
 * a pcb".  A loopback connect() pairs two kernel-internal sockets and then
 * drops both lwIP pcbs (net_inet_connect_stream -> net_tcp_drop_pcb), so a
 * pcb test would hide exactly the connections a loopback-only stack makes.
 *
 * `closed` is the teardown signal, and the invariant it buys is now per bucket
 * rather than global: net_socket_close_file unregisters the slot and sets
 * `closed` inside one bucket critical section, so every socket this walk visits
 * is still an in-registry reference holder within *its own* bucket's critical
 * section.  The walk does not promise a same-instant snapshot across buckets.  A
 * socket closed concurrently may still be seen (with `closed` already set, so
 * net_table_claims() or the row renderer drops it), but a socket whose slot has
 * already been released can never be.
 */
/* The membership rule is a20_net_table_claims() in kernel/net/socket/socket_registry.rs:
 * domain and type, not "does this socket still own a pcb".  A loopback connect()
 * drops both lwIP pcbs while the connection is live, so a pcb test would hide
 * exactly the connections a loopback-only stack makes. */

typedef struct {
    net_table_kind_t   kind;
    int                family;   /* AF_INET, AF_INET6, or AF_UNSPEC for both */
    net_table_visit_fn fn;
    void              *arg;
    int                visited;
} net_walk_arg_t;

static bool net_walk_slot(net_socket_t *s, int idx, void *arg)
{
    net_walk_arg_t *w = arg;
    (void)idx;
    /* The namespace comparison stays in C because resource_ns_current() is a
     * kernel primitive; everything it feeds -- the teardown flag, the
     * domain/type membership rule and the family narrowing -- is the Rust
     * predicate's. */
    int ns_match = s->net_ns == resource_ns_current(PROCNS_NET);
    if (!a20_net_table_walk_match((int)w->kind, w->family, s->domain, s->type,
                                  s->closed, ns_match))
        return false;
    w->visited++;
    w->fn(s, w->arg);
    return false;
}

/* Visits every socket of `kind` and `family` still owned by the table.  Returns
 * the number of visits, or -EINVAL when there is no callback to run. */
int net_socket_table_walk(net_table_kind_t kind, int family,
                          net_table_visit_fn fn, void *arg)
{
    if (!fn)
        return -EINVAL;

    net_walk_arg_t w = { kind, family, fn, arg, 0 };
    net_table_scan_all(net_walk_slot, &w);
    return w.visited;
}

/*
 * Bytes a recv() would hand over right now.  A stream socket may have a partly
 * consumed head message, so only len-off counts on it; a datagram or
 * seqpacket socket is always handed over whole.  socket_queue.c keeps the sum
 * running, so this is a load rather than a walk of the queue -- which matters
 * because the queue is per socket and the ceiling is a profile constant.
 *
 * This is the whole readable set except for the channel-backed AF_UNIX data
 * plane, where plain bytes sit in an a20_channel and net_socket_t tracks no
 * pending count for them (ch_len is the staging buffer peek()/recv() fill on
 * demand, not a backlog, so counting it would report a number that has
 * nothing to do with what is readable).
 */
static size_t net_rx_queued_locked(const net_socket_t *s)
{
    return net_rxq_bytes_locked((net_socket_t *)s);
}

int net_socket_rx_available(net_socket_t *s, size_t *out)
{
    if (!out)
        return -EINVAL;
    if (!s)
        return -ENOTSOCK;

    size_t total;
    uint64_t irq = net_sock_lock(s);
    if (!net_socket_is_live(s)) {
        net_sock_unlock(s, irq);
        return -ENOTSOCK;
    }
    /* AF_PACKET captures whole L2 frames with no stream position, so there is no
     * byte count this socket can honestly report.  Which domains those are is
     * the Rust predicate's; the refusal itself is the same errno as before. */
    int gate = a20_net_rx_available_gate(s->domain);
    if (gate != 0) {
        net_sock_unlock(s, irq);
        return gate;
    }
    total = net_rx_queued_locked(s);
    net_sock_unlock(s, irq);
    *out = total;
    return 0;
}
