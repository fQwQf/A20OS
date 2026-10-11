/* Bounded Native control-plane owner. Discovery and capability RPCs progress
 * asynchronously across peers; only an indirect probe uses a bounded helper
 * RPC. Kernel identity, routes and discovery survive daemon restarts. */
#include "liba20rt/a20_cluster_client.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_control.h"
#include "../../kernel/include/uapi/a20/cluster_gossip.h"

#ifndef A20_CLUSTERD_MEMBERS
#define A20_CLUSTERD_MEMBERS 32u
#endif
#define DIRECTORY_SERVICES 8u
#define CONTROL_CONNECTIONS 8u
#define PHASE_DIRECTORY 1u
#define PHASE_CAPABILITY 2u
#define PHASE_GOSSIP 3u
#define GOSSIP_PAGES 8u
typedef struct peer_probe {
    a20_handle_t directory, capability;
    uint32_t phase, index, count, control_slot, pending_control, steps;
    uint32_t gossip_cursor, gossip_pages;
    uint8_t node_id[16];
    uint64_t cookie, generation, epoch, deadline, next_probe;
    a20_member_service_t scratch[DIRECTORY_SERVICES];
    a20_member_service_t candidate;
} peer_probe_t;
static a20_member_t members[A20_CLUSTERD_MEMBERS];
static a20_member_service_t services[A20_CLUSTERD_MEMBERS * 2];
static peer_probe_t probes[A20_CLUSTERD_MEMBERS];
static a20_membership_t cluster;
static a20_cluster_snapshot_args_t self;
static a20_cluster_snapshot_args_t route_view[A20_CLUSTERD_MEMBERS];
static uint8_t route_seen[A20_CLUSTERD_MEMBERS];
static a20_gossip_route_t learned[A20_CLUSTERD_MEMBERS];
static a20_gossip_table_t gossip = {.routes = learned,
                                    .capacity = A20_CLUSTERD_MEMBERS};
static uint64_t route_overflow;

static void probe_close(peer_probe_t *p) {
    if (p->directory != A20_HANDLE_NULL)
        a20_hdl_close(p->directory);
    if (p->capability != A20_HANDLE_NULL)
        a20_hdl_close(p->capability);
    p->directory = p->capability = A20_HANDLE_NULL;
    p->phase = 0;
}
static a20_node_id_t member_node(unsigned i) {
    a20_node_id_t node;
    a20_memcpy(node.bytes, members[i].node_id, 16);
    return node;
}
static int track_node(const uint8_t node[16]) {
    int index = a20_member_add(&cluster, node);
    if (index < 0)
        return -1;
    peer_probe_t *p = &probes[index];
    if (a20_memcmp(p->node_id, node, 16)) {
        probe_close(p);
        *p = (peer_probe_t){.directory = A20_HANDLE_NULL,
                            .capability = A20_HANDLE_NULL};
        a20_memcpy(p->node_id, node, 16);
    }
    return index;
}

/* Local directory capacity must not turn a live peer into a failed node.
 * Keep the previous atomic snapshot until its lease expires and retry. */
static void directory_full(peer_probe_t *p, uint64_t now) {
    probe_close(p);
    p->next_probe = now + 1000;
}

static int directory_send(peer_probe_t *p) {
    uint8_t request[16];
    a20_directory_request(request, p->index, p->generation);
    p->phase = PHASE_DIRECTORY;
    return a20_channel_send_flags(p->directory, request, sizeof(request), 0, 0,
                                  A20_MSG_NONBLOCK) < 0
               ? -1
               : 0;
}

static int control_dispatch(void *ctx, const uint8_t *request, uint32_t len,
                            uint8_t *reply, uint32_t capacity) {
    (void)ctx;
    if (capacity < A20_CONTROL_REPLY)
        return -1;
    a20_memset(reply, 0, A20_CONTROL_REPLY);
    reply[0] = 'C';
    reply[1] = 'M';
    reply[2] = 1;
    a20_work_put32(reply + 4, A20_ERR_INVALID_ARGUMENT);
    if (!a20_control_valid(request, len))
        return A20_CONTROL_REPLY;
    reply[3] = request[3];
    a20_work_put64(reply + 64, a20_work_u64(request + 24));
    uint32_t index = a20_work_u32(request + 4);
    int service = -1, found = -1;
    uint64_t now = a20_agent_clock_ms(0);
    if (request[3] == A20_CONTROL_GOSSIP && a20_work_u32(request + 16) == 16 &&
        a20_work_u64(request + 24) && !a20_work_u32(request + 8) &&
        !a20_work_u32(request + 12)) {
        for (unsigned i = index; i < A20_CLUSTERD_MEMBERS; i++) {
            a20_member_t *m = &members[i];
            if (!m->state || !m->boot_epoch || !route_seen[i] ||
                !a20_gossip_advertise(&gossip, m->node_id, request + 32, now))
                continue;
            uint32_t distance = route_view[i].metric & ~A20_GOSSIP_ROUTE_MARK;
            if (distance >= A20_GOSSIP_MAX_HOPS)
                continue;
            a20_memcpy(reply + 8, m->node_id, 16);
            a20_work_put64(reply + 24, m->boot_epoch);
            a20_work_put32(reply + 32, distance);
            a20_work_put32(reply + 36, m->state);
            a20_work_put32(reply + 40, m->caps);
            a20_work_put32(reply + 44, m->profile);
            a20_work_put32(reply + 48, i + 1);
            a20_work_put64(reply + 72, m->incarnation);
            a20_work_put32(reply + 4, 0);
            return A20_CONTROL_REPLY;
        }
        a20_work_put32(reply + 4, A20_ERR_NOT_FOUND);
        return A20_CONTROL_REPLY;
    }
    if (request[3] == A20_CONTROL_RESOLVE) {
        service = a20_member_select(
            &cluster, (const char *)request + 32, a20_work_u32(request + 16),
            index, a20_work_u32(request + 8), a20_work_u32(request + 12), now);
        if (service >= 0)
            found = (int)services[service].member_index;
    } else if (request[3] == A20_CONTROL_MEMBER) {
        if (index < A20_CLUSTERD_MEMBERS && members[index].state)
            found = (int)index;
    } else if (request[3] == A20_CONTROL_INDIRECT &&
               a20_work_u32(request + 16) == 16 && a20_work_u64(request + 24)) {
        a20_node_id_t target;
        a20_memcpy(target.bytes, request + 32, 16);
        a20_directory_record_t record = {0};
        int alive = 0;
        if (!a20_memcmp(target.bytes, self.node_id, 16)) {
            record.boot_epoch = self.boot_epoch;
            record.profile = self.profile;
            record.caps = self.caps;
            alive = 1;
        } else {
            a20_handle_t ep = A20_HANDLE_NULL;
            if (a20_cluster_connect(&target, A20_DIRECTORY_SLOT, 0,
                                    A20_CONNECT_RELIABLE, 0, &ep) >= 0) {
                uint8_t query[16], wire[128];
                uint32_t n = sizeof(wire);
                a20_directory_request(query, 0, 0);
                alive =
                    a20_cluster_exchange(ep, query, sizeof(query), wire, &n,
                                         300) >= 0 &&
                    a20_directory_decode(wire, n, target.bytes, &record) == 0 &&
                    record.status != A20_DIRECTORY_INVALID;
                a20_hdl_close(ep);
            }
        }
        if (alive) {
            a20_memcpy(reply + 8, target.bytes, 16);
            a20_work_put64(reply + 24, record.boot_epoch);
            a20_work_put32(reply + 36, A20_MEMBER_ALIVE);
            a20_work_put32(reply + 40, record.caps);
            a20_work_put32(reply + 44, record.profile);
            a20_work_put32(reply + 4, 0);
        } else
            a20_work_put32(reply + 4, A20_ERR_NODE_UNREACHABLE);
        return A20_CONTROL_REPLY;
    }
    a20_work_put32(reply + 4, found >= 0 ? 0 : A20_ERR_NOT_FOUND);
    if (found < 0)
        return A20_CONTROL_REPLY;
    a20_member_t *m = &members[found];
    a20_memcpy(reply + 8, m->node_id, 16);
    a20_work_put64(reply + 24, m->boot_epoch);
    a20_work_put64(reply + 72, m->incarnation);
    a20_work_put32(reply + 36, m->state);
    a20_work_put32(reply + 40, m->caps);
    a20_work_put32(reply + 44, m->profile);
    if (service >= 0) {
        a20_member_service_t *s = &services[service];
        a20_work_put32(reply + 32, s->slot);
        a20_work_put32(reply + 48, s->max_input);
        a20_work_put32(reply + 52, s->max_output);
        a20_work_put32(reply + 60, s->max_inflight);
        for (uint32_t i = 0; i < s->operator_count; i++)
            if (s->operators[i].id == a20_work_u32(request + 8))
                a20_work_put32(reply + 56, s->operators[i].flags);
    }
    return A20_CONTROL_REPLY;
}

static int gossip_send(peer_probe_t *p) {
    uint8_t request[A20_CONTROL_REQUEST];
    a20_control_request(request, A20_CONTROL_GOSSIP, p->gossip_cursor, 0, 0,
                        (const char *)self.node_id, 16, p->cookie);
    return a20_channel_send_flags(p->directory, request, sizeof(request), 0, 0,
                                  A20_MSG_NONBLOCK) < 0
               ? -1
               : 0;
}
static void gossip_start(unsigned i, uint64_t now) {
    peer_probe_t *p = &probes[i];
    if (!(members[i].caps & A20_CLUSTER_CAP_RELAY) || !p->control_slot ||
        !route_seen[i] || route_view[i].metric != 0 ||
        route_view[i].transport_id == A20_CLX_TRANSPORT_LOOPBACK)
        return;
    a20_node_id_t node = member_node(i);
    if (a20_cluster_connect_bound(&node, p->control_slot, members[i].boot_epoch,
                                  &p->directory) < 0)
        return;
    p->phase = PHASE_GOSSIP;
    p->deadline = now + 700;
    p->gossip_pages = 0;
    if (gossip_send(p) < 0)
        directory_full(p, now);
}
static void gossip_receive(unsigned provider, const uint8_t *wire, uint32_t len,
                           uint64_t now) {
    peer_probe_t *p = &probes[provider];
    if (len != A20_CONTROL_REPLY || wire[0] != 'C' || wire[1] != 'M' ||
        wire[2] != 1 || wire[3] != A20_CONTROL_GOSSIP ||
        a20_work_u64(wire + 64) != p->cookie) {
        directory_full(p, now);
        return;
    }
    if (a20_work_u32(wire + 4)) {
        p->gossip_cursor = 0;
        directory_full(p, now);
        return;
    }
    uint32_t next = a20_work_u32(wire + 48);
    a20_gossip_hint_t hint = {.epoch = a20_work_u64(wire + 24),
                              .incarnation = a20_work_u64(wire + 72),
                              .distance = a20_work_u32(wire + 32),
                              .state = a20_work_u32(wire + 36),
                              .caps = a20_work_u32(wire + 40),
                              .profile = a20_work_u32(wire + 44)};
    a20_memcpy(hint.node, wire + 8, 16);
    if (next <= p->gossip_cursor || next > 8192u ||
        !a20_gossip_hint_valid(&hint)) {
        gossip.rejected++;
        directory_full(p, now);
        return;
    }
    int explicit_route = 0;
    for (unsigned i = 0; i < A20_CLUSTERD_MEMBERS; i++) {
        if (members[i].state &&
            a20_member_id_equal(members[i].node_id, hint.node)) {
            explicit_route = route_seen[i] &&
                             !(route_view[i].metric & A20_GOSSIP_ROUTE_MARK);
            if (hint.state != A20_MEMBER_ALIVE) {
                if (a20_member_id_equal(hint.node, self.node_id) &&
                    hint.epoch == self.boot_epoch)
                    a20_member_refute(hint.incarnation,
                                      &members[i].incarnation);
                else
                    a20_member_suspect(&cluster, i, hint.epoch,
                                       hint.incarnation, now);
            }
            break;
        }
    }
    a20_gossip_route_t proposal;
    a20_cluster_snapshot_args_t *via = &route_view[provider];
    if (!a20_gossip_propose(&gossip, self.node_id, members[provider].node_id,
                            members[provider].boot_epoch,
                            members[provider].caps, via->transport_id,
                            via->next_hop, via->next_hop_len, &hint,
                            explicit_route, now, &proposal)) {
        int index = track_node(hint.node);
        if (index >= 0) {
            a20_node_id_t node = member_node(index);
            if (a20_cluster_route(A20_ROUTE_LEARN, &node, proposal.transport,
                                  proposal.hop, proposal.hop_len,
                                  A20_ROUTE_DYNAMIC | proposal.distance) >= 0) {
                a20_gossip_commit(&gossip, &proposal);
                route_seen[index] = 1;
                route_view[index] = (a20_cluster_snapshot_args_t){
                    .transport_id = proposal.transport,
                    .next_hop_len = proposal.hop_len,
                    .metric = A20_ROUTE_DYNAMIC | proposal.distance};
                a20_memcpy(route_view[index].node_id, hint.node, 16);
                a20_memcpy(route_view[index].next_hop, proposal.hop,
                           proposal.hop_len);
            } else {
                gossip.rejected++;
            }
        } else {
            gossip.rejected++;
        }
    }
    p->gossip_cursor = next;
    if (++p->gossip_pages >= GOSSIP_PAGES || gossip_send(p) < 0)
        directory_full(p, now);
}

static int indirect(unsigned target, uint64_t cookie) {
    for (unsigned i = 0; i < A20_CLUSTERD_MEMBERS; i++) {
        if (i == target || members[i].state != A20_MEMBER_ALIVE ||
            !probes[i].control_slot ||
            !a20_memcmp(members[i].node_id, self.node_id, 16))
            continue;
        a20_node_id_t helper = member_node(i);
        a20_handle_t ep = A20_HANDLE_NULL;
        if (a20_cluster_connect(&helper, probes[i].control_slot, 0,
                                A20_CONNECT_RELIABLE, 0, &ep) < 0)
            continue;
        uint8_t request[96], reply[80];
        uint32_t len = sizeof(reply);
        a20_control_request(request, A20_CONTROL_INDIRECT, 0, 0, 0,
                            (const char *)members[target].node_id, 16, cookie);
        int ok = a20_cluster_exchange(ep, request, sizeof(request), reply, &len,
                                      450) >= 0 &&
                 len == 80 && reply[0] == 'C' && reply[1] == 'M' &&
                 reply[2] == 1 && reply[3] == A20_CONTROL_INDIRECT &&
                 !a20_work_u32(reply + 4) &&
                 a20_work_u64(reply + 64) == cookie &&
                 !a20_memcmp(reply + 8, members[target].node_id, 16);
        a20_hdl_close(ep);
        if (ok && a20_member_observe(
                      &cluster, target, cookie, a20_work_u64(reply + 24),
                      members[target].incarnation, a20_work_u32(reply + 40),
                      a20_work_u32(reply + 44), a20_agent_clock_ms(0), 0) == 0)
            return 0;
        break; /* One helper per round keeps fallback latency bounded. */
    }
    return -1;
}

static void probe_failed(unsigned i, uint64_t now) {
    peer_probe_t *p = &probes[i];
    probe_close(p);
    if (members[i].probe_cookie == p->cookie &&
        now < members[i].probe_deadline && members[i].boot_epoch &&
        indirect(i, p->cookie) == 0) {
        p->next_probe = now + 500;
        return;
    }
    members[i].probe_cookie = 0;
    if (members[i].boot_epoch)
        a20_member_suspect(&cluster, i, members[i].boot_epoch,
                           members[i].incarnation, now);
    else if (!members[i].state_deadline)
        members[i].state_deadline = now + cluster.suspect_ms;
    p->next_probe = now + 1000;
}

static void probe_start(unsigned i, uint64_t now) {
    peer_probe_t *p = &probes[i];
    p->cookie = a20_member_probe(&cluster, i, now);
    if (!p->cookie)
        return;
    p->index = p->count = p->steps = p->pending_control = 0;
    p->generation = p->epoch = 0;
    p->deadline = now + 700; /* Leave room for one indirect probe. */
    a20_node_id_t node = member_node(i);
    if (a20_cluster_connect(&node, A20_DIRECTORY_SLOT, 0, A20_CONNECT_RELIABLE,
                            0, &p->directory) < 0 ||
        directory_send(p) < 0)
        probe_failed(i, now);
}

static void probe_pump(unsigned i, uint64_t now) {
    peer_probe_t *p = &probes[i];
    if (!p->phase) {
        if (now >= p->next_probe)
            probe_start(i, now);
        return;
    }
    if (now >= p->deadline || ++p->steps > 65536) {
        if (p->phase == PHASE_GOSSIP)
            directory_full(p, now);
        else
            probe_failed(i, now);
        return;
    }
    uint8_t wire[A20_AGENT_BUFFER];
    uint32_t len = sizeof(wire), handles = 0;
    a20_handle_t ep =
        p->phase == PHASE_CAPABILITY ? p->capability : p->directory;
    a20_status_t r =
        a20_channel_recv_flags(ep, wire, &len, 0, &handles, A20_MSG_NONBLOCK);
    if (r == -A20_ERR_WOULD_BLOCK)
        return;
    if (r < 0) {
        if (p->phase == PHASE_GOSSIP)
            directory_full(p, now);
        else
            probe_failed(i, now);
        return;
    }
    if (p->phase == PHASE_GOSSIP) {
        gossip_receive(i, wire, len, now);
        return;
    }
    if (p->phase == PHASE_CAPABILITY) {
        a20_work_request_t query = {.kind = A20_WORK_QUERY};
        a20_work_result_t result;
        if (a20_work_result_decode(wire, len, &query, &result) == 0 &&
            a20_member_capabilities(&p->candidate, &result) == 0) {
            if (p->count == DIRECTORY_SERVICES) {
                directory_full(p, now);
                return;
            }
            p->scratch[p->count++] = p->candidate;
        }
        a20_hdl_close(p->capability);
        p->capability = A20_HANDLE_NULL;
        if (directory_send(p) < 0)
            probe_failed(i, now);
        return;
    }
    a20_directory_record_t record;
    if (a20_directory_decode(wire, len, members[i].node_id, &record) < 0 ||
        (p->epoch && p->epoch != record.boot_epoch)) {
        probe_failed(i, now);
        return;
    }
    if (!p->epoch) {
        if (members[i].boot_epoch != record.boot_epoch)
            p->control_slot = 0;
        if (a20_member_observe(&cluster, i, p->cookie, record.boot_epoch,
                               members[i].boot_epoch == record.boot_epoch
                                   ? members[i].incarnation
                                   : 0,
                               record.caps, record.profile, now, 1) < 0) {
            probe_failed(i, now);
            return;
        }
        p->epoch = record.boot_epoch;
    }
    if (record.status == A20_DIRECTORY_CHANGED) {
        p->index = p->count = p->pending_control = 0;
        p->generation = 0;
        if (directory_send(p) < 0)
            probe_failed(i, now);
        return;
    }
    if (record.status == A20_DIRECTORY_END) {
        if (a20_member_publish(&cluster, i, p->scratch, p->count,
                               record.generation, 5000, now) < 0) {
            directory_full(p, now);
            return;
        }
        p->control_slot = p->pending_control;
        probe_close(p);
        p->next_probe = now + 1000;
        gossip_start(i, now);
        return;
    }
    if (record.status != A20_DIRECTORY_FOUND || record.next_index <= p->index ||
        (p->generation && p->generation != record.generation)) {
        probe_failed(i, now);
        return;
    }
    p->index = record.next_index;
    p->generation = record.generation;
    if (record.name_len == 15 &&
        !a20_memcmp(record.name, "cluster.control", 15)) {
        p->pending_control = record.slot;
        if (directory_send(p) < 0)
            probe_failed(i, now);
        return;
    }
    /* A non-work service may use an unrelated protocol or never reply to
     * CW. Capability discovery must not execute an unknown service method. */
    if (!(record.flags & A20_DIRECTORY_WORK)) {
        if (directory_send(p) < 0)
            probe_failed(i, now);
        return;
    }
    p->candidate = (a20_member_service_t){.slot = record.slot,
                                          .name_len = record.name_len};
    a20_memcpy(p->candidate.name, record.name, record.name_len);
    a20_node_id_t node = member_node(i);
    if (a20_cluster_connect(&node, record.slot, 0, A20_CONNECT_RELIABLE, 0,
                            &p->capability) < 0) {
        probe_failed(i, now);
        return;
    }
    a20_work_request_t query = {.kind = A20_WORK_QUERY};
    uint8_t input[32];
    int n = a20_work_encode(input, sizeof(input), &query);
    if (a20_channel_send_flags(p->capability, input, n, 0, 0,
                               A20_MSG_NONBLOCK) < 0) {
        probe_failed(i, now);
        return;
    }
    p->phase = PHASE_CAPABILITY;
}

static int refresh_routes(void) {
    a20_cluster_snapshot_args_t route = {.kind = A20_CLUSTER_SNAPSHOT_ROUTE};
    a20_memset(route_seen, 0, sizeof(route_seen));
    for (unsigned step = 0; step < 8192; step++) {
        a20_status_t r = a20_cluster_snapshot(&route);
        if (r == -A20_ERR_NOT_FOUND)
            return 0;
        if (r == -A20_ERR_BUSY) {
            route.index = 0;
            route.generation = 0;
            a20_memset(route_seen, 0, sizeof(route_seen));
            continue;
        }
        if (r < 0)
            return -1;
        int index = track_node(route.node_id);
        if (index < 0) {
            route_overflow++;
        } else {
            route_view[index] = route;
            route_seen[index] = 1;
            a20_gossip_route_t *owned = a20_gossip_find(&gossip, route.node_id);
            if (!(route.metric & A20_ROUTE_DYNAMIC)) {
                if (owned)
                    owned->used = 0;
            } else if (!owned) {
                /* A restarted owner gives inherited dynamic routes one
                 * bounded lease to be refreshed, never permanent ownership. */
                a20_gossip_route_t recovered = {
                    .used = 1,
                    .distance = route.metric & ~A20_ROUTE_DYNAMIC,
                    .transport = route.transport_id,
                    .hop_len = route.next_hop_len,
                    .expires = a20_agent_clock_ms(0) + A20_GOSSIP_LEASE_MS};
                a20_memcpy(recovered.node, route.node_id, 16);
                a20_memcpy(recovered.hop, route.next_hop, 16);
                a20_gossip_commit(&gossip, &recovered);
            }
        }
        route.index = route.next_index;
    }
    return -1;
}
static void gossip_expire(uint64_t now) {
    for (unsigned i = 0; i < A20_CLUSTERD_MEMBERS; i++) {
        a20_gossip_route_t *r = &learned[i];
        if (!r->used || now < r->expires)
            continue;
        a20_node_id_t node;
        a20_memcpy(node.bytes, r->node, 16);
        a20_status_t status =
            a20_cluster_route(A20_ROUTE_DEL_MATCH, &node, r->transport, r->hop,
                              r->hop_len, A20_ROUTE_DYNAMIC | r->distance);
        if (status >= 0 || status == -A20_ERR_NOT_FOUND) {
            r->used = 0;
            gossip.expired++;
            for (unsigned j = 0; j < A20_CLUSTERD_MEMBERS; j++)
                if (a20_member_id_equal(members[j].node_id, node.bytes)) {
                    route_seen[j] = 0;
                    a20_member_forget_services(&cluster, j);
                }
        }
        /* BUSY also means another owner changed the route. refresh_routes
         * drops ownership of an explicit replacement; otherwise retry. */
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (a20_cluster_snapshot(&self) < 0) {
        a20_node_id_t id;
        if (argc < 2 || a20_node_id_parse_hex(argv[1], &id) < 0 ||
            a20_cluster_set_self(&id, 0) < 0 || a20_cluster_snapshot(&self) < 0)
            return 1;
    }
    if (argc >= 2) {
        a20_node_id_t id;
        if (a20_node_id_parse_hex(argv[1], &id) < 0 ||
            a20_memcmp(id.bytes, self.node_id, 16) || (argc - 2) % 2)
            return 2;
        for (int i = 2; i + 1 < argc; i += 2) {
            uint8_t hop[6] = {0};
            uint32_t transport = A20_CLX_TRANSPORT_UDP, hop_len = 6;
            if (a20_node_id_parse_hex(argv[i], &id) < 0)
                return 3;
            if (!a20_strcmp(argv[i + 1], "uart:1")) {
                transport = A20_CLX_TRANSPORT_UART;
                hop_len = 2;
                hop[0] = 1;
            } else if (a20_cluster_parse_udp(argv[i + 1], hop) < 0) {
                return 3;
            }
            if (a20_cluster_route(A20_ROUTE_REPLACE, &id, transport, hop,
                                  hop_len, 0) < 0)
                return 3;
        }
    }
    a20_membership_init(&cluster, members, A20_CLUSTERD_MEMBERS, services,
                        A20_CLUSTERD_MEMBERS * 2, 1500, 3000, 30000);
    for (unsigned i = 0; i < A20_CLUSTERD_MEMBERS; i++)
        probes[i].directory = probes[i].capability = A20_HANDLE_NULL;
    if (track_node(self.node_id) < 0 || refresh_routes() < 0)
        return 4;
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], "cluster.control",
                           A20_EXPORT_REPLACE, 0) < 0)
        return 5;
    a20_hdl_close(pair.endpoints[1]);
    a20_cluster_agent_server_t server;
    a20_work_agent_t unused = {0};
    a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
    server.dispatch = control_dispatch;
    uint64_t refresh_at = 0;
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            return 0;
        uint64_t now = a20_agent_clock_ms(0);
        if (now >= refresh_at) {
            if (refresh_routes() < 0)
                return 6;
            refresh_at = now + 1000;
        }
        gossip_expire(now);
        a20_membership_tick(&cluster, now);
        for (unsigned i = 0; i < A20_CLUSTERD_MEMBERS; i++)
            if (members[i].state)
                probe_pump(i, a20_agent_clock_ms(0));
        if (a20_cluster_agent_pump(&server) < 0)
            return 7;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
}
