/*
 * C ABI of the Rust poll readiness computation
 * (kernel/net/socket/socket_poll.rs, linked whenever the network sources are built).
 *
 * Rust owns the readiness decision: the two arms (channel-backed and legacy),
 * the HUP precedence over IN and OUT, the ERR precedence inside OUT, the
 * connected-but-peerless stream error, the connecting socket that reports
 * neither, and the deliberately weakly consistent read of the peer's queue
 * depth.
 *
 * C owns the primitives: the vfile-to-socket resolution, the socket lock, the
 * channel readable/writable/peer-closed tests, the peer queue depth read and
 * the queue capacity.
 */
#ifndef _NET_SOCKET_POLL_STATE_H
#define _NET_SOCKET_POLL_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct net_poll_view {
    void *s;
    int *domain;
    int *sock_type;
    int *closed;
    int *shut_rd;
    int *shut_wr;
    int *peer_closed;
    int *connected;
    int *local_tcp;
    int *tcp_connecting;
    int *rx_count;
    void **ch_ep;
    uint32_t *ch_len;
    void **rx_head;
    void **accept_head;
    void **peer;
} net_poll_view_t;

void *a20_net_poll_socket_from_vfile(void *vf);
void a20_net_poll_view_init(void *s, net_poll_view_t *out);
uint64_t a20_net_poll_lock(void *s);
void a20_net_poll_unlock(void *s, uint64_t flags);
int  a20_net_poll_channel_readable(void *ch_ep);
int  a20_net_poll_channel_writable(void *ch_ep);
int  a20_net_poll_channel_peer_closed(void *ch_ep);
int  a20_net_poll_peer_rx_count(void *peer);
int  a20_net_poll_max_queue(void);

int  a20_net_poll_file(void *vf, short events);

#endif /* _NET_SOCKET_POLL_STATE_H */
