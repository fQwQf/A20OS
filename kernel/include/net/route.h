/* Value-only routing transactions. IPv4 addresses retain their wire bytes;
 * tables and priorities are host order. No backend pointers escape a lookup. */
#ifndef A20_NET_ROUTE_H
#define A20_NET_ROUTE_H
#include "core/types.h"

#define NET_ROUTE_MATCH_METRIC (1u << 16)

typedef struct net_route_v4 {
    uint32_t destination;
    uint32_t gateway;
    uint32_t ifindex;
    uint32_t table;
    uint32_t metric;
    uint8_t prefix;
    uint8_t pad[3];
} net_route_v4_t;

typedef struct net_rule_v4 {
    uint32_t source;
    uint32_t destination;
    uint32_t priority;
    uint32_t table;
    uint32_t mark;
    uint32_t mark_mask;
    uint8_t source_prefix;
    uint8_t destination_prefix;
    uint8_t pad[2];
} net_rule_v4_t;

typedef struct net_route_change_v4 {
    net_route_v4_t before, after;
    uint32_t table_used;
} net_route_change_v4_t;

int a20_net_route_plan_v4(const net_route_v4_t *route, int remove,
                          uint32_t flags, net_route_change_v4_t *change);
unsigned a20_net_route_tables_v4(uint32_t src, uint32_t dst, uint32_t mark,
                                 uint32_t *out, unsigned capacity);
int a20_net_route_update_v4(const net_route_v4_t *route, int remove,
                            uint32_t flags);
int a20_net_rule_update_v4(const net_rule_v4_t *rule, int remove);
int a20_net_route_query_v4(uint32_t source, uint32_t destination, uint32_t mark,
                           net_route_v4_t *out);
unsigned a20_net_route_snapshot_v4(net_route_v4_t *out, unsigned capacity);
unsigned a20_net_rule_snapshot_v4(net_rule_v4_t *out, unsigned capacity);
/* Compatibility with existing TUN lifecycle/device-route callers. */
int a20_net_route_add(uint32_t dst, uint8_t prefix, uint16_t ifindex);
int a20_net_route_del(uint32_t dst, uint8_t prefix, uint16_t ifindex);
void a20_net_route_flush_if(uint16_t ifindex);
uint32_t a20_net_route_lookup(uint32_t dst);
#endif
