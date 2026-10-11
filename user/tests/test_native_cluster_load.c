/* Four real Native nodes send around a ring. The receiver, rather than local
 * queue admission, counts delivery inside a fixed monotonic time window. */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_cluster_client.h"

#define LOAD_BYTES 64u
#define LOAD_WARMUP 1000u
static a20_handle_t output, control_server, sink_server;
static unsigned self_id;
static uint32_t received, duplicates, bad, window_count, window_active;
/* Published once before window_active; immutable during the measurement. */
static uint64_t window_start, window_end;
/* Application-side checking is independent of the transport's replay window.
 * The sender never wraps; 1024 entries exceed its bounded outstanding span. */
static uint32_t seen[1024], highest;

static void put(const char *s) {
    a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static void number(uint64_t value) {
    char text[24];
    unsigned at = sizeof(text);
    do {
        text[--at] = '0' + value % 10;
        value /= 10;
    } while (value);
    a20_hdl_write_buf(output, text + at, sizeof(text) - at, 0);
}
static int fail(const char *why);
static uint64_t now(void) {
    a20_time_ns_t value = 0;
    if (a20_clock_get(A20_CLOCK_MONOTONIC, &value) < 0 || !value) {
        fail("clock");
        a20_task_exit(1);
    }
    return value;
}
static void pause_ms(unsigned ms) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)ms * 1000000});
}
static int fail(const char *why) {
    put("CLUSTER_LOAD: FAIL ");
    put(why);
    put("\n");
    return 1;
}

static void sink(uint64_t unused) {
    (void)unused;
    uint8_t bytes[LOAD_BYTES] = {0};
    for (;;) {
        uint32_t len = sizeof(bytes), handles = 0;
        if (a20_channel_recv(sink_server, bytes, &len, 0, &handles) < 0)
            break;
        unsigned source = a20_work_u32(bytes + 4);
        uint32_t sequence = a20_work_u32(bytes + 8);
        int valid = len == LOAD_BYTES && a20_work_u32(bytes) == 0x4c4f4144u &&
                    source == (self_id + 2) % 4 + 1 && sequence &&
                    a20_work_u32(bytes + 12) == len;
        for (unsigned i = 16; valid && i < len; i++)
            valid = bytes[i] == (uint8_t)(sequence ^ source ^ i);
        if (!valid) {
            __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (seen[sequence % 1024] == sequence) {
            __atomic_fetch_add(&duplicates, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (highest >= 1024 && sequence <= highest - 1024) {
            __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
            continue;
        }
        seen[sequence % 1024] = sequence;
        if (sequence > highest)
            highest = sequence;
        if (__atomic_load_n(&window_active, __ATOMIC_ACQUIRE) &&
            now() < window_end)
            __atomic_fetch_add(&window_count, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&received, 1, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&bad, 1, __ATOMIC_RELEASE);
    a20_thread_exit(0);
}

static void control(uint64_t unused) {
    (void)unused;
    for (;;) {
        uint8_t message[32];
        uint32_t len = sizeof(message), handles = 0;
        if (a20_channel_recv(control_server, message, &len, 0, &handles) < 0)
            break;
        int valid = len == 16 && !a20_memcmp(message, "CLLOAD1!", 8);
        uint32_t op = valid ? a20_work_u32(message + 8) : 0;
        unsigned seconds = valid ? a20_work_u32(message + 12) : 0;
        if (op == 1 && seconds && seconds <= 3600 &&
            !__atomic_load_n(&window_active, __ATOMIC_ACQUIRE)) {
            window_start = now();
            window_end = window_start + (uint64_t)seconds * 1000000000;
            __atomic_store_n(&window_active, 1, __ATOMIC_RELEASE);
        } else if (op != 2 || seconds) {
            __atomic_fetch_add(&bad, 1, __ATOMIC_RELAXED);
        }
        a20_work_put32(message, __atomic_load_n(&received, __ATOMIC_ACQUIRE));
        a20_work_put32(message + 4,
                       __atomic_load_n(&duplicates, __ATOMIC_ACQUIRE));
        a20_work_put32(message + 8, __atomic_load_n(&bad, __ATOMIC_ACQUIRE));
        a20_work_put32(message + 12,
                       __atomic_load_n(&window_count, __ATOMIC_ACQUIRE));
        a20_work_put64(message + 16, window_start);
        a20_work_put64(message + 24, window_end);
        if (a20_channel_send(control_server, message, sizeof(message), 0, 0) <
            0)
            break;
    }
    __atomic_fetch_add(&bad, 1, __ATOMIC_RELEASE);
    a20_thread_exit(0);
}

static int spawn_thread(void (*entry)(uint64_t)) {
    uint64_t stack;
    if (a20_vm_alloc_pages(16, 3, &stack) < 0)
        return -1;
    a20_thread_create_args_t args = {.entry = (uintptr_t)entry,
                                     .stack_base = stack + 16 * 4096,
                                     .stack_size = 16 * 4096};
    return a20_thread_create(&args) < 0 ? -1 : 0;
}
static int export_server(const char *name, unsigned flags,
                         a20_handle_t *server) {
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0)
        return -1;
    *server = pair.endpoints[1];
    if (a20_cluster_export(*server, name, flags, 0) < 0)
        return -1;
    a20_hdl_close(pair.endpoints[0]);
    return 0;
}
static int query(a20_handle_t ep, unsigned op, unsigned seconds,
                 uint8_t reply[32]) {
    uint8_t request[16];
    a20_memcpy(request, "CLLOAD1!", 8);
    a20_work_put32(request + 8, op);
    a20_work_put32(request + 12, seconds);
    uint32_t len = 32;
    return a20_cluster_exchange(ep, request, sizeof(request), reply, &len,
                                3000) < 0 ||
                   len != 32 || a20_work_u32(reply + 4) ||
                   a20_work_u32(reply + 8)
               ? -1
               : 0;
}
static int send_one(a20_handle_t ep, uint32_t sequence) {
    uint8_t bytes[LOAD_BYTES];
    a20_work_put32(bytes, 0x4c4f4144u);
    a20_work_put32(bytes + 4, self_id);
    a20_work_put32(bytes + 8, sequence);
    a20_work_put32(bytes + 12, sizeof(bytes));
    for (unsigned i = 16; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(sequence ^ self_id ^ i);
    a20_status_t status = a20_channel_send_flags(ep, bytes, sizeof(bytes), 0, 0,
                                               A20_MSG_NONBLOCK);
    if (status == -A20_ERR_WOULD_BLOCK)
        status = a20_channel_send(ep, bytes, sizeof(bytes), 0, 0);
    return status == -A20_ERR_WOULD_BLOCK ? 0 : status < 0 ? -1 : 1;
}
static int resources_empty(void) {
    a20_cluster_resource_args_t state = {0};
    return a20_cluster_resources(&state) >= 0 &&
           !state.values[A20_CLX_STAT_INFLIGHT] &&
           !state.values[A20_CLX_STAT_INFLIGHT_BYTES];
}
static int report_resources(void) {
    for (unsigned kind = 3; kind <= 5; kind++) {
        a20_cluster_resource_args_t state = {0};
        a20_status_t status = kind == 3 ? a20_cluster_resources(&state)
                              : kind == 4
                                  ? a20_cluster_reassembly_resources(&state)
                                  : a20_cluster_link_resources(&state);
        if (status < 0)
            return -1;
        put("CLUSTER_LOAD: resources node=");
        number(self_id);
        put(" kind=");
        number(kind);
        put(" values=");
        for (unsigned i = 0; i < 20; i++) {
            if (i)
                put(",");
            number(state.values[i]);
        }
        put("\n");
    }
    return 0;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    const a20_start_info_t *info = a20_get_start_info();
    if (!info || argc != 3)
        return 1;
    output = info->stdout_handle;
    if (a20_strlen(argv[1]) != 1 || argv[1][0] < '1' || argv[1][0] > '4')
        return fail("node");
    self_id = argv[1][0] - '0';
    unsigned seconds = 0;
    for (const char *p = argv[2]; *p; p++) {
        if (*p < '0' || *p > '9' || seconds > 3600)
            return fail("seconds");
        seconds = seconds * 10 + *p - '0';
    }
    if (!seconds || seconds > 3600)
        return fail("seconds-range");
    a20_node_id_t self = {{0}}, peer = {{0}};
    self.bytes[15] = self_id;
    peer.bytes[15] = self_id % 4 + 1;
    if (a20_cluster_set_self(&self, A20_CLUSTER_CAP_RELIABLE |
                                        A20_CLUSTER_CAP_ONEWAY) < 0 ||
        export_server("load.ctrl", 0, &control_server) < 0 ||
        export_server("load.sink", A20_EXPORT_ONEWAY, &sink_server) < 0 ||
        spawn_thread(control) < 0 || spawn_thread(sink) < 0)
        return fail("servers");
    for (unsigned id = 1; id <= 4; id++) {
        if (id == self_id)
            continue;
        a20_node_id_t node = {{0}};
        node.bytes[15] = id;
        uint8_t hop[6] = {10, 0, 3, id + 1, 0xab, 0xf4};
        if (a20_cluster_route(A20_ROUTE_ADD, &node, A20_CLX_TRANSPORT_UDP, hop,
                              sizeof(hop), 0) < 0)
            return fail("route");
    }
    put("CLUSTER_LOAD: READY node=");
    number(self_id);
    put("\n");
    a20_handle_t rpc = A20_HANDLE_NULL, events = A20_HANDLE_NULL;
    uint64_t epoch = 0, started = now();
    uint32_t slot;
    while (now() - started < 20000000000ULL) {
        if (a20_cluster_resolve(&peer, "load.ctrl", 9, 500, &slot, &epoch) >=
                0 &&
            a20_cluster_connect_bound(&peer, slot, epoch, &rpc) >= 0)
            break;
        pause_ms(10);
    }
    uint64_t sink_epoch;
    if (rpc == A20_HANDLE_NULL ||
        a20_cluster_resolve(&peer, "load.sink", 9, 3000, &slot, &sink_epoch) <
            0 ||
        sink_epoch != epoch ||
        a20_cluster_connect_bound_flags(
            &peer, slot, epoch, A20_CONNECT_RELIABLE | A20_CONNECT_ONEWAY,
            &events) < 0)
        return fail("connect");
    uint32_t submitted = 0;
    while (submitted < LOAD_WARMUP && now() - started < 30000000000ULL) {
        int result = send_one(events, submitted + 1);
        if (result < 0)
            return fail("warmup-send");
        submitted += result;
        if (!result)
            a20_thread_yield();
    }
    if (submitted != LOAD_WARMUP)
        return fail("warmup-deadline");
    uint8_t reply[32];
    do {
        if (query(rpc, 2, 0, reply) < 0)
            return fail("warmup-query");
        if (a20_work_u32(reply) == submitted && resources_empty())
            break;
        pause_ms(1);
    } while (now() - started < 30000000000ULL);
    if (a20_work_u32(reply) != submitted || !resources_empty() ||
        query(rpc, 1, seconds, reply) < 0)
        return fail("begin");
    started = now();
    uint64_t deadline = started + (uint64_t)seconds * 1000000000;
    while (now() < deadline) {
        if (submitted == 0xfffffffeu)
            return fail("sequence-limit");
        int result = send_one(events, submitted + 1);
        if (result < 0)
            return fail("send");
        submitted += result;
        if (!result)
            a20_thread_yield();
    }
    uint64_t stopped = now(), drain_deadline = stopped + 30000000000ULL;
    do {
        if (query(rpc, 2, 0, reply) < 0)
            return fail("delivery-query");
        if (a20_work_u32(reply) == submitted && resources_empty())
            break;
        pause_ms(1);
    } while (now() < drain_deadline);
    if (a20_work_u32(reply) != submitted || !resources_empty() ||
        a20_work_u64(reply + 24) - a20_work_u64(reply + 16) !=
            (uint64_t)seconds * 1000000000)
        return fail("delivery-drain");
    uint64_t drained = now();
    if (report_resources() < 0)
        return fail("resources");
    a20_hdl_close(events);
    a20_hdl_close(rpc);
    put("CLUSTER_LOAD: RESULT node=");
    number(self_id);
    put(" receiver=");
    number(self_id % 4 + 1);
    put(" submitted=");
    number(submitted - LOAD_WARMUP);
    put(" delivered=");
    number(a20_work_u32(reply) - LOAD_WARMUP);
    put(" window_admitted=");
    number(a20_work_u32(reply + 12));
    put(" window_ns=");
    number((uint64_t)seconds * 1000000000);
    put(" producer_ns=");
    number(stopped - started);
    put(" drain_ns=");
    number(drained - stopped);
    put(" duplicates=0 bad=0\n");
    /* Keep the servers alive until every node's sender has drained. */
    for (;;)
        pause_ms(1000);
}
