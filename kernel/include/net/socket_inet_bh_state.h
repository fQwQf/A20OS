/*
 * C ABI of the Rust bottom-half event ring and its per-socket consumer
 * (kernel/net/socket_inet_bh.rs, selected by NET_SOCKET_INET_IMPL=rust).
 *
 * This is the second slice of socket_inet.c: the bounded ring lwIP callbacks
 * stage inbound events into, the producer policy of every receive callback, and
 * the bottom half's per-socket consumer with its deferred FIN/RST ordering.
 *
 * Rust owns the decisions: the all-or-nothing capacity reservation and the
 * chunking of a TCP segment, the spill-versus-copy choice for a datagram, what
 * a full ring costs in each callback, which events the consumer enqueues and in
 * what order the connected/error/closed flags are applied (the ring is drained
 * against the pre-existing local close, and the peer's FIN and reset are applied
 * only afterwards), the enqueue choice per event, and the wake-batch fallback
 * when the deferred wake queue fills.
 *
 * C owns the primitives: the ring's storage and its indices, the event fields
 * (the payload area is never read by Rust -- C copies into it and the enqueue
 * takes the event as its meta), the pbuf copy/ref/free, the socket header fill
 * from the lwIP address and pktinfo, the receive-queue enqueue (inline and
 * spill), the wait-queue collect-all/one and wake-all, the event notify, the
 * socket lock, the registry slot pending mark/clear, the perf counters and the
 * socket reference/free.
 */
#ifndef _NET_SOCKET_INET_BH_STATE_H
#define _NET_SOCKET_INET_BH_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Drain bits the consumer returns; the C tail wakes these queues after the
 * deferred wake queue has been flushed. */
#define A20_NET_BH_DRAIN_READ   (1u << 0)
#define A20_NET_BH_DRAIN_WRITE  (1u << 1)
#define A20_NET_BH_DRAIN_ACCEPT (1u << 2)

/* Borrowed view of one ring event's fixed fields.  `data[]` is deliberately not
 * part of it: it is the profile-sized payload area, C fills it and hands the
 * event to the enqueue as the meta. */
typedef struct net_bh_event_view {
    void *event;
    int *type;
    int *err;
    size_t *len;
    uint8_t *addr;
    size_t *addrlen;
    uint8_t *has_pktinfo;
    uint8_t *has_hoplimit;
    uint8_t *has_tclass;
    uint32_t *pktinfo_ifindex;
    uint8_t *pktinfo_addr;
    uint8_t *hoplimit;
    uint8_t *tclass;
    void **spill;
    uint32_t *spill_off;
} net_bh_event_view_t;

void a20_net_bh_event_view(void *event, net_bh_event_view_t *out);

/* Borrowed view of the socket fields the consumer transacts over. */
typedef struct net_bh_socket_view {
    void *s;
    int *closed;
    int *peer_closed;
    int *tcp_connecting;
    int *tcp_err;
    int *connected;
    int *ever_connected;
    int *listening;
    int *reg_idx;
    volatile int *bh_connected;
    volatile int *bh_closed;
    volatile int *bh_error;
    volatile int *bh_err_code;
    volatile int *bh_tx_wake;
    volatile int *bh_pending;
} net_bh_socket_view_t;

void a20_net_bh_socket_view_init(void *s, net_bh_socket_view_t *out);

/* Primitives, defined in socket_inet.c. */
int  a20_net_bh_reserve(void *s, uint32_t n);
int  a20_net_bh_prepare(void *s, void **event_out);
void a20_net_bh_commit(void *s);
void *a20_net_bh_consume(void *s);
void a20_net_bh_consume_commit(void *s);
void a20_net_bh_reclaim_spills(void *s);
void a20_net_bh_stage_payload(void *s, void *event, void *p, uint32_t off,
                              size_t len);
uint32_t a20_net_bh_pbuf_len(void *p);
void a20_net_bh_pbuf_free(void *p);
void a20_net_bh_addr_from_ip(void *event, const void *ip, uint16_t port);
void a20_net_bh_pktinfo(void *event);
int  a20_net_bh_raw_icmp_echo(void *s, void *p);
int  a20_net_bh_enqueue_spill(void *s, void *event);
int  a20_net_bh_enqueue_inline(void *s, void *event);
int  a20_net_bh_collect_all(void *s, int which, void *wake_q);
int  a20_net_bh_collect_one(void *s, void *wake_q);
int  a20_net_bh_wakeq_count(void *wake_q);
void a20_net_bh_notify(void *s, uint32_t event, uint64_t d0, uint64_t d1);
void a20_net_bh_socket_lock(void *s, uint64_t *flags);
void a20_net_bh_socket_unlock(void *s, uint64_t flags);
int  a20_net_bh_is_live(void *s);
void a20_net_bh_socket_ref(void *s);
void a20_net_bh_socket_free(void *s);
void a20_net_bh_slot_clear(void *s);
void a20_net_bh_publish_pending(void *s);
void a20_net_bh_perf(int which);
int  a20_net_bh_accept_drain(void *s, void *wake_q);

/* The perf counters this slice counts. */
#define A20_NET_BH_PERF_OVERFLOW 0
#define A20_NET_BH_PERF_RUNS     1

/* Entry points the C wrappers call. */
int  a20_net_bh_schedule(void *s);
int  a20_net_bh_stage_tcp(void *s, void *p);
void a20_net_bh_udp_recv(void *s, void *p, const void *ip, uint16_t port);
int  a20_net_bh_raw_recv(void *s, void *p, const void *ip);
int  a20_net_bh_tcp_recv(void *s, void *p);
uint32_t a20_net_bh_process_locked(void *s, void *wake_q, int *accept_pending);
void a20_net_bh_process_socket(void *s);

#endif /* _NET_SOCKET_INET_BH_STATE_H */
