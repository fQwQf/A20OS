/*
 * C ABI of the Rust IPv4 filter state machine
 * (kernel/net/filter/netfilter_filter.rs, selected by NETFILTER_FILTER_IMPL=rust).
 *
 * Rust owns the rule table transactions, the parser, the frame walk, the
 * matching engine and the hook ordering.  C owns the storage (`g_rules`, its
 * count, the two stats banks), `g_netfilter_lock`, the rule seqlock, the
 * dotted-quad conversions and the `/proc` renderer.
 */
#ifndef _NET_NETFILTER_FILTER_STATE_H
#define _NET_NETFILTER_FILTER_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "net/netfilter.h"

typedef struct net_filter_view {
    netfilter_rule_t *rules;
    uint32_t max;
    uint32_t *count;
    netfilter_stats_t *stats_in;
    netfilter_stats_t *stats_out;
} net_filter_view_t;

/* Seqlock and conversion primitives, defined in netfilter.c. */
uint32_t a20_netfilter_rule_seq_read_begin(void);
int      a20_netfilter_rule_seq_read_retry(uint32_t start);
void     a20_netfilter_rule_seq_write_begin(void);
void     a20_netfilter_rule_seq_write_end(void);
int      a20_netfilter_ipv4_from_str(const uint8_t *s, size_t len,
                                     uint32_t *out);
/* The conntrack/NAT half, defined in netfilter_nat.c. */
int      a20_netfilter_ct_process_tail(uint8_t *frame, size_t len, int net_idx,
                                       uint8_t dir,
                                       const netfilter_frame_t *pkt);

/* Entry points the C wrappers call. */
int a20_netfilter_parse_rule(const uint8_t *line, size_t len,
                             netfilter_rule_t *out);
int a20_netfilter_rule_count(const net_filter_view_t *v);
int a20_netfilter_rule_add(const net_filter_view_t *v,
                           const netfilter_rule_t *rule);
int a20_netfilter_rule_del(const net_filter_view_t *v, uint32_t index);
void a20_netfilter_rule_reset(const net_filter_view_t *v);
int a20_netfilter_rule_get(const net_filter_view_t *v, uint32_t index,
                           netfilter_rule_t *out);
void a20_netfilter_rule_stats(const net_filter_view_t *v,
                              netfilter_stats_t *out);
int a20_netfilter_parse_frame(const uint8_t *f, size_t len,
                              netfilter_frame_t *out);
int a20_netfilter_eval(const net_filter_view_t *v, size_t len, uint8_t dir,
                       const netfilter_frame_t *pkt);
int a20_netfilter_process(const net_filter_view_t *v, uint8_t *frame,
                          size_t len, int net_idx, uint8_t dir);

#endif /* _NET_NETFILTER_FILTER_STATE_H */
