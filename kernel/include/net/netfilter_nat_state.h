/*
 * C ABI of the Rust conntrack/NAT state machine
 * (kernel/net/filter/netfilter_nat.rs, selected by NETFILTER_NAT_IMPL=rust).
 *
 * Rust owns the table transitions; C owns the storage, the locks and the real
 * I/O.  Every entry point below runs with g_lwip_lock already held (the hook
 * call sites and the timer path hold it; the procfs entry points take it
 * themselves), and none of them takes a lock of its own except the NAT table's
 * seqlock, which the Rust side brackets explicitly around the table writes and
 * reads.  Nothing here allocates and nothing blocks.
 *
 * The mirrored structs are declared in net/netfilter.h; netfilter_nat.c
 * static-asserts every offset and size so a layout change there fails the build
 * instead of corrupting this side.
 */
#ifndef _NET_NETFILTER_NAT_STATE_H
#define _NET_NETFILTER_NAT_STATE_H

#include <stddef.h>
#include <stdint.h>
#include "net/netfilter.h"

/* Borrowed view of the C-side state.  Field order is the contract with
 * `CtView` in kernel/net/filter/netfilter_nat.rs. */
typedef struct net_ct_view {
    net_conntrack_entry_t *entries;
    uint32_t max;
    uint32_t buckets;
    uint16_t *head;
    uint16_t *rhead;
    uint32_t *used;
    uint32_t *sweep;
    int32_t  *enabled;
    net_conntrack_stats_t *lifetime;
    net_conntrack_stats_t *dir_stats[2];
    net_nat_rule_t *nat;
    uint32_t nat_max;
    uint32_t *nat_count;
    uint32_t *to_ms;        /* three runtime overrides, 0 = compiled default */
    uint32_t *to_ticks;     /* three lazily derived idle limits in ticks */
    int32_t  *time_ready;
    uint32_t *sweeps;
    uint64_t ticks_per_sec; /* TICKS_PER_SEC; a runtime value on riscv64 */
} net_ct_view_t;

/* Conntrack table. */
void a20_netfilter_ct_init(const net_ct_view_t *v);
int  a20_netfilter_ct_count(const net_ct_view_t *v);
uint32_t a20_netfilter_ct_sweeps(const net_ct_view_t *v);
int  a20_netfilter_ct_lru_victim(const net_ct_view_t *v);
int  a20_netfilter_ct_inject(const net_ct_view_t *v, uint32_t src, uint32_t dst,
                             uint16_t sport, uint16_t dport, uint8_t proto,
                             uint8_t state);
int  a20_netfilter_ct_snapshot(const net_ct_view_t *v,
                               net_conntrack_entry_t *out, uint32_t max);
void a20_netfilter_ct_flush(const net_ct_view_t *v);
void a20_netfilter_ct_get_stats(const net_ct_view_t *v,
                                net_conntrack_stats_t *out);
void a20_netfilter_ct_set_enabled(const net_ct_view_t *v, int on);
int  a20_netfilter_ct_enabled(const net_ct_view_t *v);
void a20_netfilter_ct_set_timeouts(const net_ct_view_t *v, uint32_t a,
                                   uint32_t b, uint32_t c);
uint32_t a20_netfilter_ct_expire(const net_ct_view_t *v, uint32_t max_scan);
void a20_netfilter_ct_process(const net_ct_view_t *v, uint8_t *frame,
                              size_t len, int net_idx, uint8_t dir,
                              const netfilter_frame_t *pkt);

/* NAT rule table. */
int  a20_netfilter_nat_count(const net_ct_view_t *v);
int  a20_netfilter_nat_add(const net_ct_view_t *v, const net_nat_rule_t *rule);
int  a20_netfilter_nat_del(const net_ct_view_t *v, uint32_t index);
void a20_netfilter_nat_reset(const net_ct_view_t *v);
int  a20_netfilter_nat_parse(const uint8_t *line, size_t len,
                             net_nat_rule_t *out);
int  a20_netfilter_nat_pick(const net_ct_view_t *v,
                            const netfilter_frame_t *pkt, uint8_t dir,
                            net_nat_rule_t *out);

/*
 * Primitives the Rust module calls back into.  Defined in netfilter_nat.c.
 * The seqlock pair has no arguments because there is exactly one NAT table.
 */
uint32_t a20_netfilter_now_ticks(void);
uint32_t a20_netfilter_nat_seq_read_begin(void);
int      a20_netfilter_nat_seq_read_retry(uint32_t start);
void     a20_netfilter_nat_seq_write_begin(void);
void     a20_netfilter_nat_seq_write_end(void);
void     a20_netfilter_rewrite_set_addr(uint8_t *frame, size_t len,
                                        const netfilter_frame_t *pkt,
                                        uint32_t new_src, uint32_t new_dst);
void     a20_netfilter_rewrite_set_port(uint8_t *frame, size_t len,
                                        const netfilter_frame_t *pkt,
                                        int is_src, uint16_t new_port);
uint32_t a20_netfilter_masq_addr(int net_idx);
int      a20_netfilter_ipv4_from_str(const uint8_t *s, size_t len,
                                     uint32_t *out);

#endif /* _NET_NETFILTER_NAT_STATE_H */
