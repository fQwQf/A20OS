#ifndef _NET_SOCKET_SIDE_H
#define _NET_SOCKET_SIDE_H

/*
 * Per-socket state that cannot live in net_socket_t.
 *
 * net_socket_t is pinned to the profile's per-socket byte budget by a static
 * assert in socket_internal.h, so anything added to it has to displace
 * something already there.  Both tables below are indexed by the socket's
 * registry slot (net_socket_t.reg_idx) instead.
 *
 * A slot index is only a valid key while the socket holds that slot, so every
 * update and every read here runs under g_net_lock, and the registry clears a
 * slot's side state when it hands the slot out.  That is the whole safety
 * argument: no lock-free reader exists for either table.
 */

/*
 * Receive-queue byte tally (net_rxq_bytes_locked), answering FIONREAD without
 * walking the queue.
 *
 *  socket_queue.c   net_rxq_bytes_added_locked() on enqueue and on a partial
 *                   read, net_rxq_bytes_removed_locked() on a consumed
 *                   message, net_rxq_reset_slot() when a slot is allocated
 *  socket_table.c   net_rxq_bytes_locked(), the FIONREAD reader
 */
void  net_rxq_bytes_added_locked(net_socket_t *s, size_t bytes);
void  net_rxq_bytes_removed_locked(net_socket_t *s, size_t bytes);
void  net_rxq_reset_slot(int idx);
size_t net_rxq_bytes_locked(net_socket_t *s);

/*
 * Census of AF_PACKET sockets currently holding a bind filter (socket_packet.c).
 *
 *  socket_packet.c  net_packet_bound_acquire() from bind(),
 *                   net_packet_bound_release() from socket teardown
 *  socket_registry.c the teardown side, so that closing the last bound packet
 *                   socket re-disables capture for every interface
 *
 * net_packet_bound_count() is the only lock-free reader and runs on the
 * receive path with g_lwip_lock held, never g_net_lock.
 */
int  net_packet_bound_count(void);
void net_packet_bound_acquire(net_socket_t *s);
void net_packet_bound_release(net_socket_t *s);

#endif /* _NET_SOCKET_SIDE_H */