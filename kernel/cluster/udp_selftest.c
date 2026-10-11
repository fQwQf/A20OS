/* Real two-kernel acceptance fixture, gated by clxudptest=a/b.
 * This uses ordinary channel queues and the production route/UDP/proxy path;
 * tools/check-cluster-udp.py owns the independent QEMU instances. */
#include "cluster/clx_internal.h"
#include "cluster/operator.h"
#include "uapi/a20/cluster_work.h"
#include "uapi/a20/cluster_directory.h"
#include "core/bootargs.h"
#include "proc/proc.h"

static a20_channel_ep_t *udp_test_server;
static a20_channel_ep_t *udp_test_operator;
static a20_channel_ep_t *udp_test_agent;
static uint8_t udp_test_seen[96];
static uint32_t udp_test_executed, udp_test_duplicate;

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
    uint8_t buf[128];
    for (;;) {
        uint32_t len = sizeof(buf);
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
        }
        if (a20_channel_send(udp_test_server, buf, len, NULL, 0, NULL, 0) < 0)
            break;
    }
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

static void udp_test_main(void) {
    const char *args = bootargs_get();
    int side_b = args && strstr(args, "clxudptest=b");
    a20_node_id_t self = {{0}}, peer = {{0}};
    self.bytes[15] = side_b ? 2 : 1;
    peer.bytes[15] = side_b ? 1 : 2;
    uint8_t nh[6] = {10, 0, 3, side_b ? 2 : 3, 0xab, 0xf4};
    if (a20_clx_core_init() < 0 || a20_clx_self_set(&self, 0) < 0 ||
        a20_clx_route_add(&peer, A20_CLX_TRANSPORT_UDP, nh, 6, 0) < 0) {
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
    int err;
    a20_channel_ep_t *ep = a20_clx_proxy_create(&peer, 1, 0, &err);
    int ok = ep != NULL;
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
    for (uint32_t i = 0; ok && i < sizeof(udp_test_seen); i++) {
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
         stats_len == sizeof(stats) && a20_clx_get_le32(stats) == 96 &&
         a20_clx_get_le32(stats + 4) == 0;
    if (ep)
        a20_channel_ep_release(ep);
    printf("[CLX-UDP-TEST] node=%c reliable-96-at-most-once: %s\n",
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
    if (args &&
        (strstr(args, "clxudptest=a") || strstr(args, "clxudptest=b"))) {
        if (proc_alloc(udp_test_main) < 0)
            printf("[CLX-UDP-TEST] spawn: FAIL\n");
    }
}
