/*
 * C ABI of the Rust accept-staging state machine
 * (kernel/net/backends/lwip/socket_inet_accept.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * This is the first slice of socket_inet.c: the staged-accept ring, the publish
 * transaction the lwIP accept callback runs, the bottom half's drain that turns
 * a staged pcb into a registered child on the listener's accept queue, and the
 * purge a closing listener runs over its stage.
 *
 * Rust owns the decisions: whether the ring has room and what a full ring costs
 * (the drop counter), that the pcb is serviced before the slot is published,
 * the single-consumer claim, the backlog-full refusal, which slots are adopted
 * and in what order the child is wired, registered and pushed, the rollback
 * when the push fails, the `dead` handling that must neither adopt nor abort,
 * the tail publication that makes a slot recyclable, and the purge walk.
 *
 * C owns the primitives: the listener-keyed shared guard the producer takes, the
 * lwIP lock and the lane entry, the pcb callback installation and abort, the
 * pbuf walk and free, the child's socket alloc/free and namespace reference, the
 * options/peer-address inheritance, the payload staging into the child's
 * bottom-half ring, the registry register/unregister, the accept-queue push, the
 * event notify and wake-queue collect, the bottom-half schedule, the perf
 * counters and the socket lock.
 *
 * No struct ABI moves: the socket's fields are reached through a borrowed view
 * built here.
 */
#ifndef _NET_SOCKET_INET_ACCEPT_STATE_H
#define _NET_SOCKET_INET_ACCEPT_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed view of the fields the transactions read and write.  Field order is
 * the contract with `AcceptView` in kernel/net/backends/lwip/socket_inet_accept.rs; the same
 * view type is used for the listener and for the child. */
typedef struct net_accept_view {
    void *s;
    void **net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *closed;
    int *listening;
    int *nonblock;
    int *bound;
    int *connected;
    int *ever_connected;
    int *accept_count;
    int *tcp_nodelay;
    uint8_t *tcp_congestion;
    uint32_t *snd_buf;
    uint32_t *rcv_buf;
    int *keepalive;
    int *keep_idle;
    int *keep_intvl;
    int *keep_cnt;
    uint64_t *recv_timeout_ticks;
    uint64_t *send_timeout_ticks;
    uint8_t *local;
    size_t *local_len;
    void **tcp;
    int *peer_closed;
    volatile int *bh_closed;
    /* The stage, owned by the socket. */
    void *slots;
    uint32_t *head;
    uint32_t *tail;
    volatile int *dropped;
    int *drain_active;
} net_accept_view_t;

/* A staged slot, mirrored for the Rust side. */
typedef struct net_accept_slot {
    void *listener;
    void *pcb;
    void *pending;
    uint32_t fin;
    uint32_t dead;
    int error;
} net_accept_slot_t;

void a20_net_accept_view_init(void *s, net_accept_view_t *out);

/* Primitives, defined in socket_inet.c. */
/* Both guards return the flags the matching unlock must restore: the listener
 * keyed shared guard and the lwIP lock each carry the caller's IRQ state. */
uint64_t a20_net_accept_stage_lock(void *s);
void a20_net_accept_stage_unlock(void *s, uint64_t flags);
uint64_t a20_net_accept_lwip_lock(void);
void a20_net_accept_lwip_unlock(uint64_t flags);
void a20_net_accept_sock_lock(void *s, uint64_t *flags);
void a20_net_accept_sock_unlock(void *s, uint64_t flags);
int  a20_net_accept_is_live(void *s);
void a20_net_accept_slot_arm(void *slot, void *pcb);
void a20_net_accept_slot_release(void *slot);
void *a20_net_accept_alloc(void);
void a20_net_accept_free(void *s);
void a20_net_accept_ns_put(void *ns);
void *a20_net_accept_ns_get(void *ns);
void a20_net_accept_lane(void *child, void *pcb, void *listener);
void a20_net_accept_apply_options(void *child, void *pcb);
void a20_net_accept_peer_addr(void *child, void *pcb);
void *a20_net_accept_pbuf_next(void *p);
void a20_net_accept_pbuf_unlink(void *p);
void a20_net_accept_pbuf_free(void *p);
int  a20_net_accept_stage_payload(void *child, void *p);
void a20_net_accept_pcb_abort(void *pcb);
void a20_net_accept_bh_closed(void *child);
int  a20_net_accept_register(void *child);
int  a20_net_accept_unregister(void *child);
int  a20_net_accept_queue_push(void *listener, void *child);
void a20_net_accept_abort_child(void *child);
void a20_net_accept_notify_accept(void *listener, void *wake_q, int *woke);
void a20_net_accept_bh_schedule(void *s);
void a20_net_accept_race_delay(void);
void a20_net_accept_perf(int which);

/* The perf counters this slice counts. */
#define A20_NET_ACCEPT_PERF_STAGED   0
#define A20_NET_ACCEPT_PERF_QUEUED   1
#define A20_NET_ACCEPT_PERF_DROP     2
#define A20_NET_ACCEPT_PERF_ALLOC    3
#define A20_NET_ACCEPT_PERF_OVERFLOW 4

/* Entry points the C wrappers call. */
int  a20_net_accept_publish(void *listener, void *pcb, unsigned stage_size);
int  a20_net_accept_drain(void *listener, void *wake_q, unsigned stage_size,
                          int max_queue);
void a20_net_accept_purge(void *listener, unsigned stage_size);

#endif /* _NET_SOCKET_INET_ACCEPT_STATE_H */
