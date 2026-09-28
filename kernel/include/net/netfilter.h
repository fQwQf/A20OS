#ifndef _NET_NETFILTER_H
#define _NET_NETFILTER_H

#include "core/types.h"

/*
 * Packet filter for the IPv4 data plane.
 *
 * This is the first slice: an ordered rule table with drop/accept verdicts
 * plus per-rule and aggregate counters.  It deliberately has no conntrack and
 * no NAT -- those need state that outlives a single packet and are a separate
 * piece of work.
 *
 * Locking contract: both hooks are called from the lwIP data plane while
 * g_lwip_lock is held (a20_lwip_process_netif_rx_tx_locked for input,
 * a20_lwip_linkoutput for output).  Per docs/net/network-lock-contract.md a
 * hook must not block, sleep, or allocate.  The implementation therefore only
 * reads the caller's frame buffer and bumps relaxed counters; it never calls
 * into lwIP and never takes a lock of its own.
 */

#define NETFILTER_MAX_RULES 32

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

/* Verdict hooks.  `frame` is the full link-layer frame (Ethernet header first)
 * and is only valid for the duration of the call. */
netfilter_action_t netfilter_input(const void *frame, size_t len);
netfilter_action_t netfilter_output(const void *frame, size_t len);

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

#endif /* _NET_NETFILTER_H */
