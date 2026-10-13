#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uapi/a20/cluster_gossip.h"
int main(void) {
    a20_gossip_route_t routes[2] = {0}, proposal;
    a20_gossip_table_t table = {.routes = routes, .capacity = 2};
    uint8_t self[16] = {1}, provider[16] = {2}, other[16] = {3},
            hop[6] = {10, 0, 0, 2, 0xab, 0xf4};
    a20_gossip_hint_t hint = {.node = {4},
                              .epoch = 200,
                              .state = A20_MEMBER_ALIVE,
                              .caps = 4,
                              .profile = 1,
                              .distance = 0};
    assert(!a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                               0, 0, &proposal));
    assert(!routes[0].used); /* Failed kernel installation cannot publish. */
    assert(!a20_gossip_commit(&table, &proposal));
    assert(routes[0].distance == 1 && routes[0].expires == 5000);
    assert(!a20_gossip_advertise(&table, hint.node, provider, 1));
    assert(a20_gossip_advertise(&table, hint.node, other, 1));
    assert(!a20_gossip_advertise(&table, hint.node, other, 5000));
    /* Hints cannot override explicit routes, self, or an untrusted peer. */
    assert(a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                              1, 1, &proposal) < 0);
    assert(a20_gossip_propose(&table, self, provider, 100, 0, 1, hop, 6, &hint,
                              0, 1, &proposal) < 0);
    memcpy(hint.node, self, 16);
    assert(a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                              0, 1, &proposal) < 0);
    hint.node[0] = 4;
    hint.distance = 7;
    assert(a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                              0, 1, &proposal) < 0);
    hint.distance = 1;
    assert(a20_gossip_propose(&table, self, other, 101, 3, 1, hop, 6, &hint, 0,
                              1, &proposal) < 0);
    /* The same provider can increase distance; TTL bounds count-to-infinity. */
    assert(!a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                               0, 2, &proposal));
    assert(!a20_gossip_commit(&table, &proposal) && routes[0].distance == 2);
    hint.distance = 0;
    assert(!a20_gossip_propose(&table, self, other, 101, 3, 1, hop, 6, &hint, 0,
                               3, &proposal));
    assert(!a20_gossip_commit(&table, &proposal) && routes[0].provider[0] == 3);
    hint.node[0] = 5;
    assert(!a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                               0, 4, &proposal));
    assert(!a20_gossip_commit(&table, &proposal));
    hint.node[0] = 6;
    assert(a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                              0, 5, &proposal) < 0);
    routes[1].used = 0;
    assert(!a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                               0, 6, &proposal));
    hint.state = A20_MEMBER_DEAD;
    assert(a20_gossip_propose(&table, self, provider, 100, 3, 1, hop, 6, &hint,
                              0, 6, &proposal) < 0);
    puts("test_clx_gossip: transactional hints, explicit routes, bounded "
         "slots/hops, lease, split horizon and preference PASS");
    return 0;
}
