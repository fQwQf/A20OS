/*
 * C ABI of the Rust AF_PACKET state machine
 * (kernel/net/socket/socket_packet.rs, selected by NET_PACKET_IMPL=rust).
 *
 * Rust owns the capture decision and the ring protocol (full/empty, the drop
 * counter, the pending publication, the drain loop and the flush decision), the
 * delivery filter and the ancillary-address construction, the bound-socket
 * census, and the bind and send transactions.
 *
 * C owns the primitives they call: the ring spinlock and the ring's storage (the
 * slot array and the single drain slot, whose size is the profile's and whose
 * .bss footprint /proc/a20/netmem reports), the socket lock and the bucket lock,
 * the registry slot lookup, the liveness test, the initial-namespace test, the
 * receive-queue enqueue and the wakeup collect, the wake-queue init/flush, and
 * the lwIP interface queries and transmit op.
 *
 * No struct ABI moves: the socket's filter fields are reached through a borrowed
 * view built here.
 */
#ifndef _NET_SOCKET_PACKET_STATE_H
#define _NET_SOCKET_PACKET_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Borrowed view of the AF_PACKET fields the transactions read and write.
 * Field order is the contract with `PacketView` in kernel/net/socket/socket_packet.rs.
 */
typedef struct net_packet_view {
    void *s;
    void *net_ns;
    int *domain;
    int *sock_type;
    int *protocol;
    int *closed;
    int *rx_count;
    int *bound;
    uint8_t *local;
    size_t *local_len;
    int *pkt_ifindex;
    uint16_t *pkt_protocol;
    uint16_t *pkt_hatype;
    uint8_t *pkt_halen;
    uint8_t *pkt_haddr;
    int *pkt_bound;
    int *pkt_bound_marked;
} net_packet_view_t;

void a20_net_packet_view_init(void *s, net_packet_view_t *out);

/*
 * The capture ring, described rather than owned: the storage and the spinlock
 * stay in C so the profile's .bss accounting (net_packet_static_bytes()) keeps
 * describing the real layout, while every index decision is Rust's.
 */
typedef struct net_packet_ring {
    unsigned *head;
    unsigned *tail;
    volatile int *pending;
    volatile unsigned *drops;
    uint8_t *slots;
    uint8_t *drain;
    unsigned depth;
    unsigned slot_bytes;
    /* The profile's NET_MAX_QUEUE: the delivery walk refuses a socket whose
     * receive queue is already at the cap, exactly as the C version did. */
    int max_queue;
} net_packet_ring_t;

void a20_net_packet_ring_init(net_packet_ring_t *out);
void a20_net_packet_ring_lock(uint64_t *flags);
void a20_net_packet_ring_unlock(uint64_t flags);

/* Primitives, defined in socket_packet.c. */
int  a20_net_packet_ns_initial(void *ns);
int  a20_net_packet_buckets(void);
int  a20_net_packet_slots_per_bucket(void);
void a20_net_packet_bucket_lock(int bucket, uint64_t *flags);
void a20_net_packet_bucket_unlock(int bucket, uint64_t flags);
void *a20_net_packet_slot_socket(int bucket, int i);
void a20_net_packet_lock(void *s, uint64_t *flags);
void a20_net_packet_unlock(void *s, uint64_t flags);
int  a20_net_packet_is_live(void *s);
int  a20_net_packet_enqueue(void *s, const uint8_t *payload, size_t len,
                            const void *ll, size_t lllen);
int  a20_net_packet_collect_read(void *s, void *wake_q);
void a20_net_packet_wakeq_init(void *wake_q);
void a20_net_packet_wakeq_flush(void *wake_q);
int  a20_net_packet_if_up(int ifindex);
int  a20_net_packet_if_default_index(void);
int  a20_net_packet_if_hwaddr(int ifindex, uint8_t out[8]);
int  a20_net_packet_tx(int ifindex, const uint8_t *frame, size_t len);
int  a20_net_packet_ifindex_by_name(const char *name);
size_t a20_net_packet_static_bytes(void);

/* Entry points the C wrappers call. */
void a20_net_packet_rx_defer(const net_packet_ring_t *r, unsigned ifindex,
                             const uint8_t *frame, size_t len);
void a20_net_packet_bottom_half_process(const net_packet_ring_t *r);
int  a20_net_packet_rx_pending(const net_packet_ring_t *r);
int  a20_net_packet_bound_count(void);
void a20_net_packet_bound_acquire(const net_packet_view_t *v);
void a20_net_packet_bound_release(const net_packet_view_t *v);
int  a20_net_packet_socket_bind(void *s, const void *addr, size_t addrlen);
int  a20_net_packet_socket_send(void *s, const void *buf, size_t len,
                                const void *addr, size_t addrlen,
                                size_t frame_max);

#endif /* _NET_SOCKET_PACKET_STATE_H */
