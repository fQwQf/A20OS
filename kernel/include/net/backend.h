/*
 * A20OS network backend contract.
 *
 * One network domain owns exactly one backend for the lifetime of the kernel.
 * A backend is a set of *independent modules*, not one wide vtable:
 *
 *   cycle     - bring-up / teardown of the network domain
 *   event     - progress, IRQ notification and timer entry points
 *   socket    - the mandatory stream/datagram socket operations
 *   opts      - socket option policy that genuinely belongs to the stack
 *   endpoint  - optional per-socket addressing features (device bind, PMTU...)
 *   iface     - interface, address, route and neighbour transactions
 *   packet    - L2 transmit and TUN attachment
 *   obs       - read-only status and counter rendering
 *
 * Design rules (enforced by review, not by the type system):
 *
 *   1. A module pointer may be NULL.  NULL means "this backend does not
 *      implement the module", and the frontend returns -EOPNOTSUPP.  It must
 *      never mean "silently do nothing".
 *   2. `caps` describes behaviour that is visible through the Linux ABI.  A
 *      capability bit without the corresponding module is a contract bug.
 *   3. No PCB, pbuf, mbuf, lane, ifnet or driver type crosses this header.
 *      Only values, borrowed buffers and public interface indices do.
 *   4. The frontend owns fd identity, permissions, queueing and waiters.  The
 *      backend owns protocol state, flow control and protocol buffers.
 *
 * See docs/net/backend-interface.md for the module-by-module contract and
 * docs/net/instance-config.md for how an instance selects a backend.
 */
#ifndef A20_NET_BACKEND_H
#define A20_NET_BACKEND_H

#include "core/types.h"
#include "net/datagram.h"
#include "net/route.h"
#include "net/interface.h"
#include "net/net_config.h"
#include "net/tun.h"

struct net_socket;
struct device;

/* ------------------------------------------------------------------ *
 * Capabilities
 *
 * These are ABI-visible promises, not marketing.  The frontend uses them to
 * answer getsockopt()/ioctl() correctly and to reject requests before they
 * reach a backend.  A backend that cannot honour a capability must clear the
 * bit and leave the matching module NULL.
 * ------------------------------------------------------------------ */
#define NET_CAP_STREAM (1u << 0)        /* TCP/SCTP-style stream sockets   */
#define NET_CAP_DGRAM (1u << 1)         /* UDP sockets                     */
#define NET_CAP_RAW (1u << 2)           /* IPPROTO_RAW / raw sockets       */
#define NET_CAP_IPV6 (1u << 3)          /* AF_INET6 end to end             */
#define NET_CAP_LISTEN (1u << 4)        /* listen()/accept()               */
#define NET_CAP_SHUTDOWN (1u << 5)      /* half-close                      */
#define NET_CAP_SNDBUF (1u << 6)        /* SO_SNDBUF has protocol meaning  */
#define NET_CAP_RCVBUF (1u << 7)        /* SO_RCVBUF has protocol meaning  */
#define NET_CAP_PKTINFO (1u << 8)       /* IP_PKTINFO / IPV6_RECVPKTINFO   */
#define NET_CAP_BINDTODEVICE (1u << 9)  /* SO_BINDTODEVICE                 */
#define NET_CAP_UDP_CONTROL (1u << 10)  /* per-datagram ifindex/源地址     */
#define NET_CAP_PMTU (1u << 11)         /* IP_MTU_DISCOVER                 */
#define NET_CAP_RAW_TX (1u << 12)       /* raw L3 transmit                 */
#define NET_CAP_TUN (1u << 13)          /* TUN create/inject               */
#define NET_CAP_NEIGH (1u << 14)        /* neighbour insertion             */
#define NET_CAP_ROUTE (1u << 15)        /* backend RIB transaction         */
#define NET_CAP_IF_ADDR6 (1u << 16)     /* dynamic IPv6 address            */
#define NET_CAP_IF_DYNAMIC (1u << 17)   /* MTU/flags/address at runtime    */
#define NET_CAP_CONGESTION (1u << 18)   /* TCP_CONGESTION                  */
#define NET_CAP_DEVICE_IRQ (1u << 19)   /* backend consumes device IRQ     */
#define NET_CAP_MULTI_QUEUE (1u << 20)  /* more than one RX/TX queue       */
#define NET_CAP_OFFLOAD_SG (1u << 21)   /* scatter-gather transmit         */
#define NET_CAP_RAW_IPV6_TX (1u << 23) /* wire IPv6 raw transmit */
#define NET_CAP_OFFLOAD_CSUM (1u << 22) /* checksum offload negotiated     */

/* ------------------------------------------------------------------ *
 * cycle: bring the domain up, attach devices, tear it down.
 * ------------------------------------------------------------------ */
typedef struct net_backend_cycle_ops {
    /* Called once from net_init(), before any socket can exist.  Must not
     * attach NICs: drivers may not be enumerated yet. */
    void (*init)(void);
    /* Called after driver discovery.  Idempotent; may attach zero NICs. */
    void (*attach)(void);
    /* Optional.  Called only on an explicit network-domain teardown. */
    void (*shutdown)(void);
} net_backend_cycle_ops_t;

/* ------------------------------------------------------------------ *
 * event: bounded progress and wakeups.  All entry points are IRQ-safe
 * unless documented otherwise.
 * ------------------------------------------------------------------ */
typedef struct net_backend_event_ops {
    /* Drain up to `budget` device frames (0 = backend default).  Called from
     * scheduler/idle context and from the hardware IRQ bottom half. */
    void (*rx_progress)(unsigned budget);
    /* IRQ top half: acknowledge-only.  The driver has already released its
     * queue lock; the backend must not run the protocol here. */
    void (*irq)(const struct device *dev);
    /* Deliver already-staged protocol events to socket queues and waiters. */
    void (*protocol_progress)(unsigned budget);
    /* Protocol timer tick.  May be coalesced by the backend. */
    void (*timer_tick)(void);
    /* Non-zero when progress would do work.  Used to keep the scheduler's
     * bottom-half gate cheap and correct. */
    int (*work_pending)(void);
    /* Wake a blocked socket waiter without polling every socket. */
    void (*poll_waiter)(void);
} net_backend_event_ops_t;

/* ------------------------------------------------------------------ *
 * socket: mandatory operations for AF_INET/AF_INET6.
 *
 * Every function receives the frontend socket.  `transport_private` belongs
 * to the backend and is invisible to the frontend.
 * ------------------------------------------------------------------ */
typedef struct net_socket_ops {
    int (*open)(struct net_socket *s);
    void (*close)(struct net_socket *s);   /* release protocol endpoint   */
    void (*release)(struct net_socket *s); /* drop remaining attachment   */
    int (*bind)(struct net_socket *s, const void *addr, size_t len);
    int (*connect)(struct net_socket *s, const void *addr, size_t len,
                   const void *peer, size_t peer_len);
    int (*listen)(struct net_socket *s, int backlog);
    int (*send)(struct net_socket *s, const void *buf, size_t len, int flags,
                const void *addr, size_t addr_len);
    int (*shutdown)(struct net_socket *s, int receive, int transmit);
    /* The backend accepted a child into the frontend accept queue; publish
     * readiness.  Optional: the frontend still wakes accept_waitq itself. */
    void (*accept_ready)(struct net_socket *s);
    /* The frontend consumed `len` bytes; only receive-window driven stacks
     * need this.  Optional. */
    void (*recved)(struct net_socket *s, size_t len);
    /* Close/drop the protocol control block without destroying the socket. */
    void (*close_pcb)(struct net_socket *s);
    void (*drop_pcb)(struct net_socket *s);
    /* Capability queries scoped to this socket.  Optional; defaults are
     * "transport present" and cap-driven stream support. */
    int (*has_transport)(const struct net_socket *s);
    int (*has_stream)(const struct net_socket *s);
    /* Backend-specific readiness flags consumed by the poll layer. */
    unsigned (*flags)(const struct net_socket *s);
} net_socket_ops_t;

/* ------------------------------------------------------------------ *
 * opts: socket option policy owned by the stack.
 *
 * Common Linux option parsing stays in the frontend.  Only options whose
 * meaning is protocol-specific appear here.
 * ------------------------------------------------------------------ */
typedef struct net_socket_opt_ops {
    void (*apply)(struct net_socket *s, unsigned mask);
    int (*validate)(struct net_socket *s, int level, int option,
                    const void *value, size_t len);
    uint32_t (*buf_default)(int is_snd);
    uint32_t (*buf_ceiling)(struct net_socket *s, int is_snd);
    uint32_t (*buf_in_force)(struct net_socket *s, int is_snd);
    void (*ip_apply)(struct net_socket *s);
    void (*ip_effective)(struct net_socket *s, uint8_t *ttl, uint8_t *tos,
                         uint8_t *mc_ttl);
} net_socket_opt_ops_t;

/* ------------------------------------------------------------------ *
 * endpoint: optional per-socket addressing features.
 * ------------------------------------------------------------------ */
typedef struct net_endpoint_ops {
    int (*bind_device)(struct net_socket *s, const char *name);
    int (*bound_device)(struct net_socket *s, char name[16]);
    int (*udp_control)(struct net_socket *s, const void *buf, size_t len,
                       int flags, const void *addr, size_t addr_len,
                       const net_datagram_control_t *control);
    int (*pmtu)(struct net_socket *s, int mode, int set);
    int (*raw_send)(struct net_socket *s, const void *buf, size_t len,
                    const void *addr, size_t addr_len, int hdrincl,
                    int protocol);
    int (*ioctl)(struct net_socket *s, unsigned long request, void *arg);
} net_endpoint_ops_t;

/* ------------------------------------------------------------------ *
 * iface: interface, address, route and neighbour transactions.
 *
 * All addresses keep the byte order their caller uses: wire order for
 * netlink/route and host-order IPv4 only where explicitly named.
 * ------------------------------------------------------------------ */
typedef struct net_backend_if_ops {
    int (*up)(unsigned index);
    int (*default_index)(void);
    int (*owns_ipv4)(uint32_t wire_address);
    int (*mtu)(unsigned index);
    int (*hwaddr)(unsigned index, uint8_t mac[6]);
    int (*snapshot)(void *out, int maximum);
    int (*snapshot_addr6)(nl_addr6_out_t *out, int maximum);
    void (*config_snapshot)(a20_net_config_t *out);
    int (*set_mtu)(unsigned index, uint16_t mtu);
    int (*set_flags)(unsigned index, uint32_t value, uint32_t mask);
    int (*get_addr)(unsigned index, uint8_t *addr, uint8_t *mask,
                    uint8_t *gateway);
    int (*set_addr)(unsigned index, const uint8_t *addr, const uint8_t *mask,
                    const uint8_t *gateway);
    int (*set_addr6)(unsigned index, const uint8_t *addr, uint8_t prefix,
                     uint32_t valid, uint32_t preferred, int exclusive);
    int (*del_addr6)(unsigned index, const uint8_t *addr);
    int (*policy_update_v4)(const void *route, int remove, uint32_t flags);
    int (*route_update_v6)(const net_route_v6_t *route, int remove, uint32_t flags);
    int (*rule_update_v6)(const net_rule_v6_t *rule, int remove);
    unsigned (*route_snapshot_v6)(net_route_v6_t *out, unsigned capacity);
    unsigned (*rule_snapshot_v6)(net_rule_v6_t *out, unsigned capacity);
    int (*route_lookup_v6)(const uint8_t src[16], const uint8_t dst[16],
                           unsigned index, unsigned table, nl_route6_out_t *out);
    int (*route_update_v4)(const uint8_t *dst, uint8_t prefix,
                           const uint8_t *gateway, uint16_t ifindex,
                           int remove);
    int (*neighbor_update_v4)(uint32_t ifindex, uint32_t address,
                              const uint8_t mac[6]);
    uint32_t (*interface_ipv4)(int index);
} net_backend_if_ops_t;

/* ------------------------------------------------------------------ *
 * packet: L2 transmit and TUN attachment.  These bypass the socket
 * protocol path on purpose; AF_PACKET and TUN own their framing.
 * ------------------------------------------------------------------ */
typedef struct net_backend_packet_ops {
    int (*tx)(unsigned index, const uint8_t *frame, size_t len);
    int (*tun_create)(net_tun_emit_t emit, void *ctx);
    void (*tun_destroy)(unsigned index);
    int (*tun_inject)(unsigned index, const uint8_t *packet, size_t len);
} net_backend_packet_ops_t;

/* ------------------------------------------------------------------ *
 * obs: read-only rendering.  Never mutates protocol state and never
 * blocks on a socket lock.
 * ------------------------------------------------------------------ */
typedef struct net_backend_obs_ops {
    int (*status)(char *buf, size_t len);
    int (*memory)(char *buf, size_t len);
    int (*devices)(char *buf, size_t len);
    int (*routes)(char *buf, size_t len);
} net_backend_obs_ops_t;

/* ------------------------------------------------------------------ *
 * The backend descriptor.
 * ------------------------------------------------------------------ */
typedef struct net_backend {
    const char *name; /* stable: "lwip", "native", "bsd" */
    uint32_t caps;
    const net_backend_cycle_ops_t *cycle;
    const net_backend_event_ops_t *event;
    const net_socket_ops_t *socket;
    const net_socket_opt_ops_t *opts;
    const net_endpoint_ops_t *endpoint;
    const net_backend_if_ops_t *iface;
    const net_backend_packet_ops_t *packet;
    const net_backend_obs_ops_t *obs;
} net_backend_t;

/* ------------------------------------------------------------------ *
 * Registry and selection.
 *
 * Registration happens once from net_init().  Each compiled backend exports
 * a20_<name>_backend_register(); the build defines CONFIG_NET_BACKEND_<NAME>
 * only for sources that are actually linked.  Selection is explicit: an
 * unknown name is an error, never a fallback.
 * ------------------------------------------------------------------ */
#define NET_BACKEND_MAX 4

/* Internal: called once from net_stack_init(). */
void net_backend_registry_init(void);
int net_backend_register(const net_backend_t *backend);
const net_backend_t *net_backend_find(const char *name);
/* Select by name; NULL selects CONFIG_NET_BACKEND_NAME (from the instance
 * TOML, baked in by the build).  Returns -ENODEV when not compiled in. */
int net_backend_select(const char *name);
/* The selected descriptor is write-once at net_init() and read on every
 * dispatch.  Keeping the accessor inline removes one call per socket op from
 * the hot path without adding a per-packet branch. */
extern const net_backend_t *a20_net_backend_active;
static inline const net_backend_t *net_backend_current(void) {
    return a20_net_backend_active;
}
/* Non-zero when the selected backend advertises every bit in `caps`. */
int net_backend_has(uint32_t caps);
/* Render "lwip:stream,dgram,..." for /proc/net/status. */
int net_backend_format_caps(char *buf, size_t len);

#endif /* A20_NET_BACKEND_H */
