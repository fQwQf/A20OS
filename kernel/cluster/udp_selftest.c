/* Real independent-kernel acceptance fixture, gated by clxudptest=a/b/r.
 * This uses ordinary channel queues and the production route/UDP/proxy path;
 * tools/check-cluster-udp.py owns the independent QEMU instances. */
#include "cluster/clx_internal.h"
#include "cluster/operator.h"
#include "uapi/a20/cluster_work.h"
#include "uapi/a20/cluster_directory.h"
#include "core/bootargs.h"
#include "proc/proc.h"
#include "mm/slab.h"
#include "lwip/stats.h"

static a20_channel_ep_t *udp_test_server;
static a20_channel_ep_t *udp_test_operator;
static a20_channel_ep_t *udp_test_agent;
static uint8_t udp_test_seen[1000];
static uint32_t udp_test_executed, udp_test_duplicate;
static uint32_t udp_slow_executed;
static uint8_t udp_large_seen[8];
static uint32_t udp_large_executed, udp_large_duplicate;
static int udp_large_peer_done;

static void udp_test_work(void) {
    const a20_work_operator_t ops[] = {
        {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 1024, 8, a20_work_dot, NULL},
        {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 1024, 8,
         a20_work_sum64, NULL},
    };
    const a20_work_agent_t agent = {.operators = ops,
                                    .operator_count = 2,
                                    .max_input = 1024,
                                    .max_output = 1024,
                                    .max_inflight = 1,
                                    .max_budget_ms = 30000};
    uint8_t request[1056], reply[1056];
    for (;;) {
        uint32_t len = sizeof(request);
        if (a20_channel_recv(udp_test_agent, request, &len, NULL, NULL, NULL,
                             0) < 0)
            break;
        int n = a20_work_dispatch(&agent, request, len, reply, sizeof(reply));
        if (n < 0 ||
            a20_channel_send(udp_test_agent, reply, n, NULL, 0, NULL, 0) < 0)
            break;
    }
    a20_channel_ep_release(udp_test_agent);
}

static void udp_test_dot(void) {
    uint8_t request[256], reply[A20_CLX_DOT_REPLY_BYTES];
    for (;;) {
        uint32_t len = sizeof(request);
        if (a20_channel_recv(udp_test_operator, request, &len, NULL, NULL, NULL,
                             0) < 0)
            break;
        int n = a20_clx_operator_dot(request, len, reply, sizeof(reply));
        if (n < 0 ||
            a20_channel_send(udp_test_operator, reply, n, NULL, 0, NULL, 0) < 0)
            break;
    }
    a20_channel_ep_release(udp_test_operator);
}

static void udp_test_echo(void) {
    uint8_t *buf = kmalloc(CLX_LOGICAL_MTU);
    if (!buf)
        return;
    for (;;) {
        uint32_t len = CLX_LOGICAL_MTU;
        if (a20_channel_recv(udp_test_server, buf, &len, NULL, NULL, NULL, 0) <
            0)
            break;
        if (len == 64 && a20_clx_get_le32(buf) == 0x72656c79u) {
            uint32_t index = a20_clx_get_le32(buf + 4);
            if (index < sizeof(udp_test_seen)) {
                if (udp_test_seen[index])
                    udp_test_duplicate++;
                else {
                    udp_test_seen[index] = 1;
                    udp_test_executed++;
                }
            }
        } else if (len == 8 && a20_clx_get_le32(buf) == 0x73746174u) {
            a20_clx_put_le32(buf, udp_test_executed);
            a20_clx_put_le32(buf + 4, udp_test_duplicate);
        } else if (len == 8 && a20_clx_get_le32(buf) == 0x776f6c73u) {
            udp_slow_executed++;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10000));
        } else if (len == 8 && a20_clx_get_le32(buf) == 0x74736c73u) {
            a20_clx_put_le32(buf, udp_slow_executed);
        }
        if (len > 1400 && a20_clx_get_le32(buf) == 0x67617266u) {
            unsigned index = a20_clx_get_le32(buf + 4);
            if (index < sizeof(udp_large_seen)) {
                if (udp_large_seen[index])
                    udp_large_duplicate++;
                else {
                    udp_large_seen[index] = 1;
                    udp_large_executed++;
                }
            }
        } else if (len == 8 && a20_clx_get_le32(buf) == 0x74736766u) {
            a20_clx_put_le32(buf, udp_large_executed);
            a20_clx_put_le32(buf + 4, udp_large_duplicate);
        }
        if (len == 8 && a20_clx_get_le32(buf) == 0x656e6466u)
            __atomic_store_n(&udp_large_peer_done, 1, __ATOMIC_RELEASE);
        if (a20_channel_send(udp_test_server, buf, len, NULL, 0, NULL, 0) < 0)
            break;
    }
    kfree(buf);
    a20_channel_ep_release(udp_test_server);
}

static int udp_test_roundtrip(a20_channel_ep_t *ep, const uint8_t *request,
                              uint32_t len) {
    uint8_t reply[128];
    uint32_t got = sizeof(reply);
    return ep && len <= sizeof(reply) &&
           a20_channel_send(ep, request, len, NULL, 0, NULL, 0) == 0 &&
           a20_channel_recv(ep, reply, &got, NULL, NULL, NULL, 0) == 0 &&
           got == len && memcmp(reply, request, len) == 0;
}

static int udp_test_large(const a20_node_id_t *peer) {
    uint32_t heap_before =
        __atomic_load_n(&lwip_stats.mem.used, __ATOMIC_RELAXED);
    uint32_t maximum = a20_clx_logical_mtu(peer) - 54;
    if (maximum < 3002)
        return 0;
    const uint32_t sizes[] = {1423, 1430,        2000,        2048,
                              3000, maximum - 2, maximum - 1, maximum};
    uint8_t *input = kmalloc(maximum), *reply = kmalloc(maximum);
    int err, ok = input && reply;
    a20_channel_ep_t *ep =
        ok ? a20_clx_proxy_create(peer, 1, A20_CONNECT_RELIABLE, &err) : NULL;
    ok = ok && ep;
    for (unsigned i = 0; ok && i < 8; i++) {
        for (unsigned j = 0; j < sizes[i]; j++)
            input[j] = (uint8_t)(j * 73 + i);
        a20_clx_put_le32(input, 0x67617266u);
        a20_clx_put_le32(input + 4, i);
        uint32_t got = maximum;
        int sent = a20_channel_send(ep, input, sizes[i], NULL, 0, NULL, 0);
        int received =
            sent == 0 ? a20_channel_recv(ep, reply, &got, NULL, NULL, NULL, 0)
                      : -1;
        ok = sent == 0 && received == 0 && got == sizes[i] &&
             memcmp(input, reply, got) == 0;
        printf("[CLX-UDP-TEST] large peer=%u index=%u len=%u send=%d recv=%d "
               "got=%u\n",
               peer->bytes[15], i, sizes[i], sent, received, got);
    }
    uint8_t stats[8] = {0};
    a20_clx_put_le32(stats, 0x74736766u);
    uint32_t got = sizeof(stats);
    ok = ok &&
         a20_channel_send(ep, stats, sizeof(stats), NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, stats, &got, NULL, NULL, NULL, 0) == 0 &&
         got == sizeof(stats) && a20_clx_get_le32(stats) == 8 &&
         a20_clx_get_le32(stats + 4) == 0;
    a20_clx_put_le32(stats, 0x656e6466u);
    got = sizeof(stats);
    ok = ok &&
         a20_channel_send(ep, stats, sizeof(stats), NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, stats, &got, NULL, NULL, NULL, 0) == 0;
    uint64_t wait_start = timer_get_ticks();
    while (ok && !__atomic_load_n(&udp_large_peer_done, __ATOMIC_ACQUIRE) &&
           timer_get_ticks() - wait_start < MS_TO_TICKS(15000))
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
    ok = ok && __atomic_load_n(&udp_large_peer_done, __ATOMIC_ACQUIRE);
    if (ep)
        a20_channel_ep_release(ep);
    kfree(input);
    kfree(reply);
    /* Late duplicated pieces are incomplete assemblies with a finite age. */
    proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(2300));
    a20_cluster_resource_args_t resources;
    a20_clx_fragment_resources(&resources);
    ok = ok && !resources.values[A20_CLX_REASM_CONTEXTS] &&
         !resources.values[A20_CLX_REASM_BYTES] &&
         resources.values[A20_CLX_REASM_COMPLETED] >= 14 &&
         resources.values[A20_CLX_REASM_PEAK_BYTES] > 1400;
    uint32_t heap_after =
        __atomic_load_n(&lwip_stats.mem.used, __ATOMIC_RELAXED);
    printf("[CLX-UDP-TEST] heap peer=%u before=%u after=%u reassembly=%lu/%lu "
           "complete=%lu\n",
           peer->bytes[15], heap_before, heap_after,
           (unsigned long)resources.values[A20_CLX_REASM_CONTEXTS],
           (unsigned long)resources.values[A20_CLX_REASM_BYTES],
           (unsigned long)resources.values[A20_CLX_REASM_COMPLETED]);
    /* Allow a few asynchronous ARP/heartbeat buffers, not a growing TX heap. */
    ok = ok && heap_after <= heap_before + 8192;
    return ok;
}

static void udp_test_main(void) {
    const char *args = bootargs_get();
    unsigned rounds = args && strstr(args, "clxudpstress=1") ? 1000 : 96;
    int side_b = args && strstr(args, "clxudptest=b");
    int relay = args && strstr(args, "clxudptest=r");
    int routed = args && strstr(args, "clxudprouted=1");
    a20_node_id_t self = {{0}}, peer = {{0}};
    self.bytes[15] = relay ? 3 : side_b ? 2 : 1;
    peer.bytes[15] = side_b ? 1 : 2;
    uint8_t nh[6] = {10, 0, 3, routed ? 4 : side_b ? 2 : 3, 0xab, 0xf4};
    if (a20_clx_core_init() < 0 ||
        a20_clx_self_set(&self, A20_CLUSTER_CAP_RELIABLE |
                                    (relay ? A20_CLUSTER_CAP_RELAY : 0)) < 0) {
        printf("[CLX-UDP-TEST] setup: FAIL\n");
        return;
    }
    if (relay) {
        for (unsigned i = 1; i <= 2; i++) {
            peer.bytes[15] = i;
            nh[3] = i + 1;
            if (a20_clx_route_add(&peer, A20_CLX_TRANSPORT_UDP, nh, 6, 0) < 0) {
                printf("[CLX-UDP-TEST] relay-setup: FAIL\n");
                return;
            }
        }
        printf("[CLX-UDP-TEST] relay-ready: PASS\n");
        return;
    }
    a20_node_id_t gateway = {{0}};
    gateway.bytes[15] = 3;
    if ((routed &&
         a20_clx_route_add(&gateway, A20_CLX_TRANSPORT_UDP, nh, 6, 0) < 0) ||
        a20_clx_route_add(&peer, A20_CLX_TRANSPORT_UDP, nh, 6, routed ? 1 : 0) <
            0) {
        printf("[CLX-UDP-TEST] setup: FAIL\n");
        return;
    }
    udp_test_server = a20_channel_create(8, NULL);
    uint32_t slot;
    if (!udp_test_server ||
        a20_clx_export_register_owned(udp_test_server, "echo", 4, 0, &slot) <
            0 ||
        slot != 1 || proc_alloc(udp_test_echo) < 0) {
        printf("[CLX-UDP-TEST] export: FAIL\n");
        return;
    }
    udp_test_operator = a20_channel_create(8, NULL);
    if (!udp_test_operator ||
        a20_clx_export_register_owned(udp_test_operator, "dot", 3, 0, &slot) <
            0 ||
        slot != 2 || proc_alloc(udp_test_dot) < 0) {
        printf("[CLX-UDP-TEST] operator-export: FAIL\n");
        return;
    }
    udp_test_agent = a20_channel_create(8, NULL);
    if (!udp_test_agent ||
        a20_clx_export_register_owned(udp_test_agent, "work", 4,
                                      A20_EXPORT_WORK, &slot) < 0 ||
        slot != 3 || proc_alloc(udp_test_work) < 0) {
        printf("[CLX-UDP-TEST] agent-export: FAIL\n");
        return;
    }
    if (args && strstr(args, "clxudpreboot=1")) {
        printf("[CLX-UDP-TEST] reboot-server-ready\n");
        return;
    }
    uint32_t state = 0;
    uint32_t rtt;
    uint64_t tx, rx, tx_drop, rx_drop, retransmit, hello_age;
    for (int i = 0; i < 1000; i++) {
        a20_clx_link_status_fill(&peer, &state, &rtt, &tx, &rx, &tx_drop,
                                 &rx_drop, &retransmit, &hello_age);
        if (state == A20_CLX_LINK_UP)
            break;
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(20));
    }
    if (state != A20_CLX_LINK_UP) {
        printf("[CLX-UDP-TEST] handshake: FAIL\n");
        return;
    }
    if (routed) {
        uint64_t local, remote;
        uint32_t caps;
        int confirmed = 0;
        for (unsigned i = 0; i < 1000; i++) {
            if (!a20_clx_peer_session(&peer, &local, &remote, &caps)) {
                confirmed = (caps & A20_CLX_CAP_FRAGMENT) != 0;
                break;
            }
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(20));
        }
        printf("[CLX-UDP-TEST] node=%c routed-fragment-session: %s\n",
               side_b ? 'b' : 'a', confirmed ? "PASS" : "FAIL");
        if (!confirmed)
            return;
    }
    int err;
    /* HELLO completion can be asymmetric. A reliable probe establishes that
     * both ends accept data before deliberately exercising lossy v0 calls. */
    a20_channel_ep_t *ep =
        a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
    const uint8_t ready[] = "ready";
    int ok = udp_test_roundtrip(ep, ready, sizeof(ready));
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c session-ready: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    if (!ok)
        return;
    ep = a20_clx_proxy_create(&peer, 1, 0, &err);
    ok = ep != NULL;
    for (uint32_t i = 0; ok && i < 8; i++) {
        uint8_t request[64], reply[64];
        for (uint32_t j = 0; j < sizeof(request); j++)
            request[j] = (uint8_t)(i * 17 + j + side_b);
        uint32_t len = sizeof(reply);
        ok = a20_channel_send(ep, request, sizeof(request), NULL, 0, NULL, 0) ==
                 0 &&
             a20_channel_recv(ep, reply, &len, NULL, NULL, NULL, 0) == 0 &&
             len == sizeof(request) && memcmp(reply, request, len) == 0;
    }
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c echo-8: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
    ok = ep != NULL;
    for (uint32_t i = 0; ok && i < rounds; i++) {
        uint8_t request[64] = {0}, reply[64];
        a20_clx_put_le32(request, 0x72656c79u);
        a20_clx_put_le32(request + 4, i);
        uint32_t len = sizeof(reply);
        ok = a20_channel_send(ep, request, sizeof(request), NULL, 0, NULL, 0) ==
                 0 &&
             a20_channel_recv(ep, reply, &len, NULL, NULL, NULL, 0) == 0 &&
             len == sizeof(request) && memcmp(reply, request, len) == 0;
    }
    uint8_t stats[8] = {0};
    a20_clx_put_le32(stats, 0x73746174u);
    uint32_t stats_len = sizeof(stats);
    ok = ok &&
         a20_channel_send(ep, stats, sizeof(stats), NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, stats, &stats_len, NULL, NULL, NULL, 0) == 0 &&
         stats_len == sizeof(stats) && a20_clx_get_le32(stats) == rounds &&
         a20_clx_get_le32(stats + 4) == 0;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c reliable-%u-at-most-once: %s\n",
           side_b ? 'b' : 'a', rounds, ok ? "PASS" : "FAIL");
    a20_cluster_resource_args_t observed = {0};
    if (args && strstr(args, "clxudpcorrupt=1")) {
        a20_clx_link_resources(&observed);
        printf("[CLX-UDP-TEST] node=%c crc-counter: %s malformed=%lu\n",
               side_b ? 'b' : 'a',
               observed.values[A20_CLX_LINK_RX_MALFORMED] ? "PASS" : "FAIL",
               (unsigned long)observed.values[A20_CLX_LINK_RX_MALFORMED]);
    }
    if (args && strstr(args, "clxudpflood=1")) {
        uint64_t wait = timer_get_ticks();
        do {
            a20_clx_link_resources(&observed);
            if (observed.values[A20_CLX_LINK_RX_DROPS] >= 1000)
                break;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
        } while (timer_get_ticks() - wait < MS_TO_TICKS(5000));
        uint64_t links = observed.values[A20_CLX_LINK_COUNT];
        uint64_t drops = observed.values[A20_CLX_LINK_RX_DROPS];
        a20_clx_resources(&observed);
        /* A failed pre-session send can also create the adapter-0 bucket. */
        ok = drops >= 1000 && links <= 2 &&
             observed.values[A20_CLX_STAT_ROUTES] == 1 &&
             observed.values[A20_CLX_STAT_EXPORTS] == 3 &&
             observed.values[A20_CLX_STAT_ORIGINS] == 1;
        printf("[CLX-UDP-TEST] node=%c hello-flood-bounded: %s drops=%lu "
               "links=%lu routes=%lu origins=%lu\n",
               side_b ? 'b' : 'a', ok ? "PASS" : "FAIL", (unsigned long)drops,
               (unsigned long)links,
               (unsigned long)observed.values[A20_CLX_STAT_ROUTES],
               (unsigned long)observed.values[A20_CLX_STAT_ORIGINS]);
    }
    ok = udp_test_large(&peer);
    a20_clx_link_status_fill(&peer, &state, &rtt, &tx, &rx, &tx_drop, &rx_drop,
                             &retransmit, &hello_age);
    printf("[CLX-UDP-TEST] after-large peer=%u state=%u age=%lu tx=%lu rx=%lu "
           "drops=%lu/%lu\n",
           peer.bytes[15], state, (unsigned long)hello_age, (unsigned long)tx,
           (unsigned long)rx, (unsigned long)tx_drop, (unsigned long)rx_drop);
    printf("[CLX-UDP-TEST] node=%c fragmented-8-at-most-once: %s\n",
           side_b ? 'b' : 'a', ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 2, 0, &err);
    uint8_t request[28] = {0}, reply[A20_CLX_DOT_REPLY_BYTES];
    a20_clx_put_le32(request, 3);
    const int32_t a[] = {1, -2, 3}, b[] = {4, 5, -6};
    for (uint32_t i = 0; i < 3; i++) {
        a20_clx_put_le32(request + 4 + i * 4, (uint32_t)a[i]);
        a20_clx_put_le32(request + 16 + i * 4, (uint32_t)b[i]);
    }
    uint32_t reply_len = sizeof(reply);
    ok =
        ep &&
        a20_channel_send(ep, request, sizeof(request), NULL, 0, NULL, 0) == 0 &&
        a20_channel_recv(ep, reply, &reply_len, NULL, NULL, NULL, 0) == 0 &&
        reply_len == sizeof(reply) && a20_clx_get_le32(reply) == 0 &&
        (int64_t)a20_clx_get_le64(reply + 4) == -24;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c shared-dot: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 3, A20_CONNECT_RELIABLE, &err);
    uint8_t work_request[128], work_reply[128];
    a20_work_request_t work = {.kind = A20_WORK_QUERY};
    int work_len = a20_work_encode(work_request, sizeof(work_request), &work);
    reply_len = sizeof(work_reply);
    ok = ep &&
         a20_channel_send(ep, work_request, work_len, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, work_reply, &reply_len, NULL, NULL, NULL, 0) ==
             0 &&
         reply_len == 88 && a20_work_u32(work_reply + 20) == A20_WORK_OK &&
         a20_work_u32(work_reply + 32) == A20_WORK_CAP_OPERATOR;
    work = (a20_work_request_t){.kind = A20_WORK_OPERATOR,
                                .job = 77,
                                .shard = 5,
                                .operator_id = A20_WORK_DOT,
                                .budget_ms = 1000,
                                .input_len = sizeof(request),
                                .attempt = 3,
                                .input = request};
    work_len = a20_work_encode(work_request, sizeof(work_request), &work);
    reply_len = sizeof(work_reply);
    ok = ok &&
         a20_channel_send(ep, work_request, work_len, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, work_reply, &reply_len, NULL, NULL, NULL, 0) ==
             0 &&
         reply_len == 40 && a20_work_u32(work_reply + 20) == A20_WORK_OK &&
         a20_work_u64(work_reply + 4) == 77 &&
         a20_work_u32(work_reply + 12) == 5 &&
         a20_work_u32(work_reply + 28) == 3 &&
         (int64_t)a20_work_u64(work_reply + 32) == -24;
    work.kind = A20_WORK_PROCESS;
    work_len = a20_work_encode(work_request, sizeof(work_request), &work);
    reply_len = sizeof(work_reply);
    ok = ok &&
         a20_channel_send(ep, work_request, work_len, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, work_reply, &reply_len, NULL, NULL, NULL, 0) ==
             0 &&
         reply_len == 32 &&
         a20_work_u32(work_reply + 20) == A20_WORK_UNSUPPORTED;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c unified-work: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, A20_DIRECTORY_SLOT, A20_CONNECT_RELIABLE,
                              &err);
    uint8_t query[A20_DIRECTORY_REQUEST], directory_wire[A20_DIRECTORY_REPLY];
    uint32_t index = 0, work_slot = 0;
    uint64_t generation = 0;
    ok = ep != NULL;
    for (unsigned step = 0; ok && step < 4; step++) {
        a20_directory_request(query, index, generation);
        reply_len = sizeof(directory_wire);
        a20_directory_record_t record;
        ok =
            a20_channel_send(ep, query, sizeof(query), NULL, 0, NULL, 0) == 0 &&
            a20_channel_recv(ep, directory_wire, &reply_len, NULL, NULL, NULL,
                             0) == 0 &&
            a20_directory_decode(directory_wire, reply_len, peer.bytes,
                                 &record) == 0;
        if (!ok)
            break;
        if (step == 3) {
            ok = record.status == A20_DIRECTORY_END && work_slot == 3;
        } else {
            ok = record.status == A20_DIRECTORY_FOUND &&
                 record.next_index > index;
            index = record.next_index;
            generation = record.generation;
            if (record.name_len == 4 && !memcmp(record.name, "work", 4))
                work_slot = record.slot;
        }
    }
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c directory-discovery: %s\n",
           side_b ? 'b' : 'a', ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 0x7fffffff, 0, &err);
    uint8_t byte = 42;
    uint32_t len = 1;
    ok = ep && a20_channel_send(ep, &byte, 1, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL, 0) < 0;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c unknown-slot: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 0x7fffffff, A20_CONNECT_RELIABLE, &err);
    len = 1;
    ok = ep && a20_channel_send(ep, &byte, 1, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL, 0) ==
             -A20_ERR_CANCELED;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c reliable-unknown-slot: %s\n",
           side_b ? 'b' : 'a', ok ? "PASS" : "FAIL");
    ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
    len = 1;
    ok = ep && a20_channel_send(ep, NULL, 0, NULL, 0, NULL, 0) == 0 &&
         a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL, 0) == 0 &&
         len == 0;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c empty-call: %s\n", side_b ? 'b' : 'a',
           ok ? "PASS" : "FAIL");
    uint8_t oversize[2048] = {0};
    ep = a20_clx_proxy_create(&peer, 1, 0, &err);
    len = 1;
    ok = ep &&
         a20_channel_send(ep, oversize, sizeof(oversize), NULL, 0, NULL, 0) ==
             0 &&
         a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL, 0) ==
             -A20_ERR_CANCELED;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c oversize-completes: %s\n",
           side_b ? 'b' : 'a', ok ? "PASS" : "FAIL");
    if (args && strstr(args, "clxudpfaults=1")) {
        uint8_t lost[8] = {0};
        a20_clx_put_le32(lost, 0x626c616bu);
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        uint64_t started = timer_get_ticks();
        len = 1;
        ok = ep &&
             a20_channel_send(ep, lost, sizeof(lost), NULL, 0, NULL, 0) == 0 &&
             a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL, 0) ==
                 -A20_ERR_CANCELED &&
             timer_get_ticks() - started <=
                 MS_TO_TICKS(CLX_CALL_DEADLINE_MS + 500);
        if (ep)
            a20_channel_ep_release(ep);
        printf("[CLX-UDP-TEST] node=%c bounded-blackhole: %s\n",
               side_b ? 'b' : 'a', ok ? "PASS" : "FAIL");
    }
    if (!side_b && args && strstr(args, "clxudpslow=1")) {
        uint8_t slow[8] = {0};
        a20_clx_put_le32(slow, 0x776f6c73u);
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        uint64_t started = timer_get_ticks();
        ok = ep &&
             a20_channel_send(ep, slow, sizeof(slow), NULL, 0, NULL, 0) == 0;
        int64_t received = -A20_ERR_WOULD_BLOCK;
        while (ok && timer_get_ticks() - started < MS_TO_TICKS(2000)) {
            len = sizeof(slow);
            received = a20_channel_recv(ep, slow, &len, NULL, NULL, NULL,
                                        A20_MSG_NONBLOCK);
            if (received != -A20_ERR_WOULD_BLOCK)
                break;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(5));
        }
        /* The Native job driver enforces an attempt budget by closing its
         * endpoint; this exercises the same retirement during execution. */
        ok = ok && received == -A20_ERR_WOULD_BLOCK &&
             timer_get_ticks() - started <= MS_TO_TICKS(2500);
        if (ep)
            a20_channel_ep_release(ep);
        proc_sleep_until(started + MS_TO_TICKS(12000));
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        a20_clx_put_le32(slow, 0x74736c73u);
        len = sizeof(slow);
        ok = ok && ep &&
             a20_channel_send(ep, slow, sizeof(slow), NULL, 0, NULL, 0) == 0 &&
             a20_channel_recv(ep, slow, &len, NULL, NULL, NULL, 0) == 0 &&
             len == sizeof(slow) && a20_clx_get_le32(slow) == 1;
        if (ep)
            a20_channel_ep_release(ep);
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(200));
        a20_clx_resources(&observed);
        ok = ok && !observed.values[A20_CLX_STAT_INFLIGHT] &&
             !observed.values[A20_CLX_STAT_INFLIGHT_BYTES];
        printf("[CLX-UDP-TEST] node=a slow-consumer-recovered: %s\n",
               ok ? "PASS" : "FAIL");
    }
    if (!side_b && args && strstr(args, "clxudppartition=1")) {
        printf("[CLX-UDP-TEST] node=a partition-ready: PASS\n");
        uint64_t started = timer_get_ticks();
        proc_sleep_until(started + MS_TO_TICKS(250));
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        uint8_t isolated[8] = {0};
        ok = ep && a20_channel_send(ep, isolated, sizeof(isolated), NULL, 0,
                                    NULL, 0) == 0;
        int saw_suspect = 0;
        int64_t received = -A20_ERR_WOULD_BLOCK;
        while (ok && timer_get_ticks() - started <= MS_TO_TICKS(15000)) {
            a20_clx_link_resources(&observed);
            saw_suspect |= observed.values[A20_CLX_LINK_SUSPECT_COUNT] != 0;
            len = sizeof(isolated);
            received = a20_channel_recv(ep, isolated, &len, NULL, NULL, NULL,
                                        A20_MSG_NONBLOCK);
            if (received != -A20_ERR_WOULD_BLOCK)
                break;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
        }
        ok = ok && saw_suspect && received == -A20_ERR_CANCELED &&
             timer_get_ticks() - started <= MS_TO_TICKS(15000);
        if (ep)
            a20_channel_ep_release(ep);
        printf("[CLX-UDP-TEST] node=a partition-canceled: %s\n",
               ok ? "PASS" : "FAIL");
        proc_sleep_until(started + MS_TO_TICKS(32000));
        for (unsigned i = 0; ok && i < 1000; i++) {
            a20_clx_link_status_fill(&peer, &state, &rtt, &tx, &rx, &tx_drop,
                                     &rx_drop, &retransmit, &hello_age);
            if (state == A20_CLX_LINK_UP)
                break;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(10));
        }
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        const uint8_t recovered[] = "partition-recovered";
        ok = ok && state == A20_CLX_LINK_UP &&
             udp_test_roundtrip(ep, recovered, sizeof(recovered));
        if (ep)
            a20_channel_ep_release(ep);
        printf("[CLX-UDP-TEST] node=a partition-recovered: %s\n",
               ok ? "PASS" : "FAIL");
    }
    if (!side_b && args && strstr(args, "clxudprestart=1")) {
        uint64_t local_session, previous_session, new_session = 0;
        uint32_t caps;
        ep = a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err);
        const uint8_t probe[] = "before-reboot";
        ok = a20_clx_peer_session(&peer, &local_session, &previous_session,
                                  &caps) == 0 &&
             udp_test_roundtrip(ep, probe, sizeof(probe));
        printf("[CLX-UDP-TEST] reboot-armed: %s\n", ok ? "PASS" : "FAIL");
        int closed = 0;
        for (uint32_t i = 0; ok && i < 1000; i++) {
            len = 1;
            int64_t r = a20_channel_recv(ep, &byte, &len, NULL, NULL, NULL,
                                         A20_MSG_NONBLOCK);
            if (r == -A20_ERR_CANCELED)
                closed = 1;
            if (closed &&
                a20_clx_peer_session(&peer, &local_session, &new_session,
                                     &caps) == 0 &&
                new_session != previous_session)
                break;
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(20));
        }
        if (ep)
            a20_channel_ep_release(ep);
        ok = ok && closed && new_session && new_session != previous_session;
        ep = ok ? a20_clx_proxy_create(&peer, 1, A20_CONNECT_RELIABLE, &err)
                : NULL;
        const uint8_t fresh_probe[] = "after-reboot";
        ok = ok && udp_test_roundtrip(ep, fresh_probe, sizeof(fresh_probe));
        printf("[CLX-UDP-TEST] reboot-session-ready: %s\n",
               ok ? "PASS" : "FAIL");
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(700));
        memset(stats, 0, sizeof(stats));
        a20_clx_put_le32(stats, 0x73746174u);
        stats_len = sizeof(stats);
        ok =
            ok &&
            a20_channel_send(ep, stats, sizeof(stats), NULL, 0, NULL, 0) == 0 &&
            a20_channel_recv(ep, stats, &stats_len, NULL, NULL, NULL, 0) == 0 &&
            stats_len == sizeof(stats) && a20_clx_get_le32(stats) == 0 &&
            a20_clx_get_le32(stats + 4) == 0;
        if (ep)
            a20_channel_ep_release(ep);
        printf("[CLX-UDP-TEST] restart-old-session-rejected: %s\n",
               ok ? "PASS" : "FAIL");
    }
}

void a20_clx_udp_selftest_boot(void) {
    const char *args = bootargs_get();
    if (args && (strstr(args, "clxudptest=a") || strstr(args, "clxudptest=b") ||
                 strstr(args, "clxudptest=r"))) {
        if (proc_alloc(udp_test_main) < 0)
            printf("[CLX-UDP-TEST] spawn: FAIL\n");
    }
}
