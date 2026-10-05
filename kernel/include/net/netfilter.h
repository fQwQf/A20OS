#ifndef _NET_NETFILTER_H
#define _NET_NETFILTER_H

#include "core/types.h"

/*
 * Packet filter and NAT for the IPv4 data plane.
 *
 * Three layers, in the order a packet meets them:
 *
 *   1. An ordered rule table with drop/accept verdicts plus per-rule and
 *      aggregate counters.
 *   2. conntrack: a fixed-size 5-tuple table with TCP/UDP flow state
 *      (NEW/ESTABLISHED), idle-timeout reclamation, LRU eviction at capacity,
 *      and counters.  Always on; an untracked packet still gets its filter
 *      verdict, it just has no state.
 *   3. NAT: SNAT/MASQUERADE on the output hook and DNAT on the input hook,
 *      with the translated tuple remembered in the conntrack entry so the
 *      reply direction is rewritten back.
 *
 * Locking contract: both hooks are called from the lwIP data plane while
 * g_lwip_lock is held (a20_lwip_process_netif_rx_tx_locked for input,
 * a20_lwip_linkoutput for output).  Per docs/net/network-lock-contract.md a
 * hook must not block, sleep, or allocate.  The implementation therefore only
 * reads and rewrites the caller's frame buffer and bumps relaxed counters; it
 * never calls into lwIP and never takes a lock of its own.
 *
 * The conntrack table is protected by g_lwip_lock, not by g_netfilter_lock,
 * and this is the one place where adding a second lock would have been wrong.
 * A dedicated conntrack spinlock looked like the tidier answer, but the packet
 * path already holds g_lwip_lock, so the new lock would have added a second
 * global acquisition per packet today and, once g_lwip_lock is sharded
 * (docs/net/net-lanes.md), become the new serialisation point in its place --
 * the exact outcome the lane work exists to avoid.  So every mutation happens
 * from a context that already holds g_lwip_lock: the two hooks, and the sweep
 * called from a20_lwip_poll_timers_locked().  The only mutator that does not is
 * the /proc flush, and it takes g_lwip_lock itself (a20_lwip_lock), which is
 * safe because a procfs write is a syscall and never runs under it.  Readers
 * from /proc take the same lock rather than a seqlock, because the table is
 * mutated in place with no copy-out step to bracket.
 *
 * The frame buffer is what makes NAT possible here at all: the hooks sit
 * below lwIP, on the raw Ethernet frame, so a translation is an in-place
 * header rewrite plus a checksum fixup.  That also bounds what NAT can be --
 * see the honesty notes at the bottom of this header.
 *
 * HONEST BOUNDARIES.  These are limits of this implementation, not of the
 * design, and each one is a thing a caller can trip over:
 *
 *   - No ALG.  There is no FTP/SIP/ISAKMP payload inspection, so a protocol
 *     that carries its addresses in the body is translated only in the
 *     headers and its control channel will not follow.
*   - No ICMP tracking.  Only TCP and UDP create conntrack entries.  An ICMP
*     error is therefore not matched against a flow, and an ICMP error
*     quoting an untracked flow does not create one.  Path MTU discovery, which
*     depends on ICMP error tracking, does not work through this.
*   - Flow state is inferred from the tuple and the TCP flags byte only.  A flow
*     is ESTABLISHED once a reply is seen, or once a forward packet carries ACK
*     without SYN.  There is no sequence-number window check, no RST/FIN teardown
*     (a closed flow lingers until its idle timeout), and no half-open limit
*     separate from the table size.
 *   - No helper modules, no fullcone/port-preserving variants beyond what is
 *     listed below, and no NAT of IPv6 (the hook parses IPv4 only).
 *   - Translation applies to unfragmented packets and to first fragments.
 *     A non-first fragment carries no ports, so it is never translated; a
 *     fragmented flow can therefore be silently mangled.  Fragmented NAT is
 *     not implemented.
 *   - The checksum fixup is the standard incremental update of the IP header
 *     checksum and the L4 pseudo-header checksum.  It does not verify the
 *     incoming checksum, so a frame that arrived with a bad checksum keeps a
 *     correspondingly bad one.
 */

#define NETFILTER_MAX_RULES 32
#define NETFILTER_MAX_NAT_RULES 32

/* Match-all sentinels.  Distinguished from a real 0.0.0.0 / port 0 rule. */
#define NETFILTER_NO_ADDR 0xFFFFFFFFu
#define NETFILTER_NO_PORT 0xFFFFu

#define NETFILTER_PROTO_ANY  0
#define NETFILTER_PROTO_ICMP 1
#define NETFILTER_PROTO_TCP  6
#define NETFILTER_PROTO_UDP  17

typedef enum {
    NETFILTER_DIR_IN = 0,
    NETFILTER_DIR_OUT = 1
} netfilter_dir_t;

typedef enum {
    NETFILTER_ACCEPT = 0,
    NETFILTER_DROP = 1
} netfilter_action_t;

typedef struct {
    uint8_t  dir;       /* netfilter_dir_t */
    uint8_t  proto;     /* NETFILTER_PROTO_* */
    uint8_t  action;    /* netfilter_action_t */
    uint16_t src_port;  /* host order, NETFILTER_NO_PORT = any */
    uint16_t dst_port;  /* host order, NETFILTER_NO_PORT = any */
    uint32_t src_addr;  /* host order, NETFILTER_NO_ADDR = any */
    uint32_t dst_addr;  /* host order, NETFILTER_NO_ADDR = any */
    uint64_t matched;
    uint64_t bytes;
} netfilter_rule_t;

typedef struct {
    uint64_t in_packets;
    uint64_t out_packets;
    uint64_t in_dropped;
    uint64_t out_dropped;
    uint64_t in_accepted;
    uint64_t out_accepted;
} netfilter_stats_t;

/*
 * Verdict hooks.  `frame` is the full link-layer frame (Ethernet header first)
 * and is only valid for the duration of the call.  `net_idx` is the index of
 * the netif the frame came from or is going to (netif_get_index); MASQUERADE
 * needs it because it takes its address from the interface it is leaving.
 *
 * NAT rewrites the frame in place, so the buffer is `uint8_t *` rather than
 * const: a DNAT'd inbound frame must reach lwIP with the translated
 * destination, and an SNAT'd outbound frame must reach the wire with the
 * translated source.  Both hooks are called on the frame buffer the caller is
 * about to hand onward, so the rewrite is visible to the next stage.
 */
netfilter_action_t netfilter_input(uint8_t *frame, size_t len, int net_idx);
netfilter_action_t netfilter_output(uint8_t *frame, size_t len, int net_idx);

/*
 * Parsed IPv4 view of a frame.  Shared between the rule evaluator and the
 * NAT rewriter so the Ethernet/VLAN/IPv4/L4 walk exists once -- a rewriter
 * that re-derived its own offsets would be free to disagree with the matcher
 * about where the header ends.
 *
 * `l4_off` is 0 when there is no usable L4 header (not TCP/UDP, a non-first
 * fragment, or the frame is too short).  NAT refuses to translate in that
 * case; see HONEST BOUNDARIES above.
 */
typedef struct {
    uint32_t src_addr;    /* host order */
    uint32_t dst_addr;    /* host order */
    uint16_t src_port;    /* host order, NETFILTER_NO_PORT if not TCP/UDP */
    uint16_t dst_port;
    uint8_t  proto;       /* NETFILTER_PROTO_* or the raw IP protocol byte */
    uint8_t  has_ports;   /* 1 when src_port/dst_port are real */
    uint16_t ip_off;      /* offset of the IPv4 header in the frame */
    uint16_t l4_off;      /* offset of the L4 header, 0 if none */
    uint16_t tcp_flags;   /* TCP flags byte 0, 0 otherwise */
} netfilter_frame_t;

/* Returns 0 when the frame is not IPv4 (including VLAN-tagged ARP and IPv6)
 * or is truncated.  Never reads past `len`. */
int netfilter_parse_frame(const uint8_t *frame, size_t len, netfilter_frame_t *out);

/* Rule table management.  With no rules installed the default policy is
 * ACCEPT, so an unconfigured system behaves exactly as before. */
int         netfilter_rule_count(void);
int         netfilter_add_rule(const netfilter_rule_t *rule);
int         netfilter_del_rule(unsigned index);
void        netfilter_reset(void);
int         netfilter_get_rule(unsigned index, netfilter_rule_t *out);
void        netfilter_get_stats(netfilter_stats_t *out);
void        netfilter_format(char *buf, size_t bufsz);

/* Parse one "add" line into a rule.  Accepts:
 *   in|out proto=<n|any|icmp|tcp|udp> src=<ip|any> dst=<ip|any>
 *              sport=<n|any> dport=<n|any> action=accept|drop
 * Addresses and ports are decimal or dotted-quad; "any" means match all.
 * Returns 0, or -EINVAL / -ERANGE. */
int netfilter_parse_rule(const char *line, size_t len, netfilter_rule_t *out);

/* ---------------------------------------------------------------- conntrack */

/*
 * Per-flow state, consulted by the hooks after the filter has returned a
 * verdict; rewrites `frame` in place.  Not for direct use --
 * netfilter_input/netfilter_output call it.  Exported only so the ordering
 * (filter first, then conntrack, then NAT) is visible in one place rather than
 * spread across the hooks.
 */
void netfilter_conntrack_process(uint8_t *frame, size_t len, int net_idx,
                                 netfilter_dir_t dir,
                                 const netfilter_frame_t *pkt);

/* Enable or disable tracking.  Tracking is on by default; turning it off
 * leaves the filter untouched and only stops entries being created.  Existing
 * entries are kept so that turning it back on does not reset the NAT bindings
 * that flows still depend on. */
void netfilter_conntrack_set_enabled(int on);
int  netfilter_conntrack_enabled(void);

typedef enum {
    NET_CONNTRACK_NEW = 0,
    NET_CONNTRACK_ESTABLISHED = 1
} net_conntrack_state_t;

/*
 * One tracked flow, keyed by the original-direction 5-tuple.
 *
 * `nat_*` holds the translation this flow was given, so the reply direction
 * can be rewritten back without re-running the NAT rules (which may have
 * changed, or been deleted, since).  A flow with nat == NET_NAT_NONE is
 * tracked but not translated -- that is the common case for a host that only
 * wants stateful accounting.
 *
 * The trailing link field is a table internal, visible only because the table
 * is a static array of this type and /proc hands the same struct to its
 * reader.  NET_CONNTRACK_NONE is the "no entry" sentinel in the chain and in
 * every accessor that returns an index.
 *
 * There is no LRU list: `last_ms` is the LRU key, and eviction at capacity is
 * a linear scan for its minimum.  That is deliberately the dumb version --
 * it costs one extra pass over the table only when a *new* flow arrives while
 * the table is already full, and in exchange there is no second set of links
 * that every insert and every eviction has to keep consistent.
 */
typedef struct {
    uint32_t src_addr;      /* host order */
    uint32_t dst_addr;
    uint16_t src_port;      /* host order, NETFILTER_NO_PORT if not TCP/UDP */
    uint16_t dst_port;
    uint8_t  proto;
    uint8_t  state;         /* net_conntrack_state_t */
    uint8_t  nat;           /* net_nat_type_t, NET_NAT_NONE if untranslated */
    uint8_t  dir;           /* netfilter_dir_t of the packet that created it */
    uint32_t nat_src_addr;  /* SNAT/MASQ: translated source */
    uint32_t nat_dst_addr;  /* DNAT: translated destination */
    uint16_t nat_src_port;  /* translated source port, 0 = unchanged */
    uint16_t nat_dst_port;
    uint32_t last_ms;       /* idle-timeout reference and LRU key */
    uint64_t packets;
    uint64_t bytes;
    uint16_t hash_next;     /* index of the next entry in this bucket */
} net_conntrack_entry_t;

#define NET_CONNTRACK_NONE 0xFFFFu

typedef struct {
    uint64_t tracked;      /* live entries */
    uint64_t created;
    uint64_t evicted;      /* dropped by LRU at capacity */
    uint64_t timeout;      /* reclaimed by idle timeout */
    uint64_t packets;
    uint64_t bytes;
} net_conntrack_stats_t;

/* ---------------------------------------------------------------------- NAT */

typedef enum {
    NET_NAT_NONE = 0,
    NET_NAT_SNAT = 1,
    NET_NAT_MASQUERADE = 2,
    NET_NAT_DNAT = 3
} net_nat_type_t;

/*
 * A NAT rule.  Same shape and same parser as a filter rule, so the control
 * surface reads the same way; the difference is that these are consulted only
 * for the first packet of a flow, and the result is recorded in the conntrack
 * entry rather than applied per packet.
 *
 * MASQUERADE takes its address from the outgoing netif at translation time,
 * which is why it carries no address here; SNAT and DNAT require one.
 *
 * HONEST LIMIT: ports are never remapped.  `nat_port` of 0 means "keep the
 * original port", and a non-zero `nat_port` is applied verbatim -- there is no
 * allocator picking a free source port.  This is the right default for the
 * two cases that matter here (container egress masquerade, where the
 * translated address is already unique per flow; and port forwarding, where
 * the forwarded port is unique by construction), but a host that translated
 * many flows onto one address and one port would have them collapse onto a
 * single binding.
 */
typedef struct {
    uint8_t  dir;          /* netfilter_dir_t */
    uint8_t  proto;        /* NETFILTER_PROTO_* */
    uint8_t  nat;          /* net_nat_type_t */
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t nat_addr;     /* required for SNAT and DNAT */
    uint16_t nat_port;     /* 0 = keep the original port */
    uint64_t matched;
    uint64_t bytes;
} net_nat_rule_t;

int  netfilter_nat_rule_count(void);
int  netfilter_nat_add_rule(const net_nat_rule_t *rule);
int  netfilter_nat_del_rule(unsigned index);
void netfilter_nat_reset(void);

/* Append the conntrack and NAT sections to a buffer already holding the filter
 * section, starting at `off`.  Split out of netfilter_format() so each half of
 * the module owns its own text and the 4096-byte budget is clamped in one
 * place. */
void netfilter_nat_format(char *buf, size_t bufsz, size_t off);

/*
 * Parse one "add" line into a NAT rule.  Same direction/proto/src/dst/sport/
 * dport keys as a filter rule, plus:
 *     action=snat|masquerade|dnat    what to do
 *     to=<ip>                       target address (snat, dnat)
 *     toport=<n>                    target port; 0 or absent keeps the original
 * Returns 0, or -EINVAL / -ERANGE.  A rule that names snat or dnat without
 * `to=` is rejected at parse time rather than becoming a rule that silently
 * matches and does nothing.
 */
int  netfilter_nat_parse_rule(const char *line, size_t len, net_nat_rule_t *out);

/*
 * Table inspection and management.  All of these require g_lwip_lock held,
 * including the /proc-facing ones -- see the locking note at the top of this
 * header.  The lookup and insert primitives are internal to the packet path
 * (they mutate entries in place, so a caller outside the module would have no
 * way to keep its copy consistent); what is exposed here is what /proc needs
 * plus the sweeper.
 */

/* Bring the table up.  Called from a20_lwip_init(); the table must not be
 * walked before it, because a zeroed bucket head is a valid slot index and the
 * chain would never terminate. */
void netfilter_conntrack_init(void);

/* Copy out up to `max` live entries; returns how many were written.  Used by
 * /proc, which is given a 4096-byte buffer, so the caller bounds `max` too. */
int netfilter_conntrack_snapshot(net_conntrack_entry_t *out, unsigned max);

int  netfilter_conntrack_count(void);
void netfilter_conntrack_flush(void);
void netfilter_conntrack_get_stats(net_conntrack_stats_t *out);

/*
 * Reclaim entries idle for longer than the per-state timeout.  Called from the
 * timer path with g_lwip_lock held; returns the number reclaimed.
 *
 * `max_scan` bounds the work per call and the scan resumes from where the last
 * call stopped, so a full table cannot make one timer tick unbounded.
 */
unsigned netfilter_conntrack_expire(unsigned max_scan);

/*
 * Idle timeouts, in milliseconds.  Split by protocol and state because the
 * two lifetimes are genuinely different: a half-open TCP flow that never
 * completes its handshake must not pin a table slot for the length of an
 * established connection's lifetime, and a UDP "flow" is often a single
 * datagram whose peer will never answer.
 */
#define NET_CONNTRACK_TIMEOUT_TCP_NEW_MS        30000u
#define NET_CONNTRACK_TIMEOUT_TCP_ESTABLISHED_MS 120000u
#define NET_CONNTRACK_TIMEOUT_UDP_MS            30000u

#endif /* _NET_NETFILTER_H */
