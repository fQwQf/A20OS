#ifndef _NET_SOCKET_SIDE_H
#define _NET_SOCKET_SIDE_H

/*
 * Per-socket state that used to be addressed by registry slot.
 *
 * Both the receive-queue byte tally and the AF_PACKET bind census were
 * NET_MAX_SOCKETS-entry tables indexed by net_socket_t.reg_idx -- 512 KiB and
 * 8 KiB respectively on the server profile.  Being addressed by slot is also
 * what made a single global lock (g_net_lock) the only thing keeping them
 * coherent, so any second socket touching them serialized against every other
 * socket in the system.
 *
 * Both now live in net_socket_t (rxq_tally, pkt_bound_marked) and are written
 * only under that socket's own bucket lock, so a table walk no longer has to
 * be excluded from a socket's data path.  What is left here is the small
 * surface that still has to be shared: the AF_PACKET counter, whose only reader
 * is the lwIP receive path and is therefore deliberately lock-free.
 */

/*
 * Receive-queue byte tally (net_rxq_bytes_locked), answering FIONREAD without
 * walking the queue.
 *
 *  socket_queue.c   net_rxq_bytes_added_locked() on enqueue and on a partial
 *                   read, net_rxq_bytes_removed_locked() on a consumed
 *                   message, net_rxq_reset_locked() when a slot is allocated
 *  socket_table.c   net_rxq_bytes_locked(), the FIONREAD reader
 */
void  net_rxq_bytes_added_locked(net_socket_t *s, size_t bytes);
void  net_rxq_bytes_removed_locked(net_socket_t *s, size_t bytes);
void  net_rxq_reset_locked(net_socket_t *s);
size_t net_rxq_bytes_locked(net_socket_t *s);

/*
 * Census of AF_PACKET sockets currently holding a bind filter (socket_packet.c).
 *
 * The per-socket marker is net_socket_t.pkt_bound_marked, written under that
 * socket's bucket lock by both sides; what stays global is the counter.
 *
 *  socket_packet.c  net_packet_bound_acquire() from bind(),
 *                   net_packet_bound_release() from socket teardown
 *  socket_registry.c the teardown side, so that closing the last bound packet
 *                   socket re-disables capture for every interface
 *
 * net_packet_bound_count() is the only lock-free reader and runs on the
 * receive path with g_lwip_lock held, never a bucket lock.
 */
int  net_packet_bound_count(void);
void net_packet_bound_acquire(net_socket_t *s);
void net_packet_bound_release(net_socket_t *s);

#endif /* _NET_SOCKET_SIDE_H */
