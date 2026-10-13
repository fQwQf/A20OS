/* Native fused RPC measurements on local, loopback and independent UDP
 * endpoints. Samples exclude reporting; every returned byte is checked. */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_cluster.h"

#define BENCH_MAX_CALLS 100000u
#define BENCH_WARMUP 100u
#define BENCH_BYTES 65536u

static a20_handle_t output, server;
static uint8_t server_bytes[BENCH_BYTES], request[BENCH_BYTES],
    reply[BENCH_BYTES];
static uint64_t samples[BENCH_MAX_CALLS], blocks[100];
/* The echo thread alone owns this counter, including stats serialization. */
static uint64_t server_calls;
static uint32_t server_failed;
static int serving_network;

static void put(const char *s) {
    a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}

static void number(uint64_t value) {
    char bytes[24];
    unsigned n = sizeof(bytes);
    do {
        bytes[--n] = '0' + value % 10;
        value /= 10;
    } while (value);
    a20_hdl_write_buf(output, bytes + n, sizeof(bytes) - n, 0);
}

static uint64_t now(void) {
    a20_time_ns_t time = 0;
    if (a20_clock_get(A20_CLOCK_MONOTONIC, &time) < 0)
        return 0;
    return time;
}

static void pause_ms(unsigned ms) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)ms * 1000000});
}

static int fail(const char *why) {
    put("CLUSTER_BENCH: FAIL ");
    put(why);
    put("\n");
    return 1;
}

static void resources(void) {
    for (unsigned kind = 3; kind <= 5; kind++) {
        a20_cluster_resource_args_t stats = {0};
        a20_status_t status = kind == 3 ? a20_cluster_resources(&stats)
                              : kind == 4
                                  ? a20_cluster_reassembly_resources(&stats)
                                  : a20_cluster_link_resources(&stats);
        if (status < 0)
            continue;
        put("CLUSTER_BENCH: resources kind=");
        number(kind);
        put(" values=");
        for (unsigned i = 0; i < 20; i++) {
            if (i)
                put(",");
            number(stats.values[i]);
        }
        put("\n");
    }
}

static void echo(uint64_t ignored) {
    (void)ignored;
    for (;;) {
        uint32_t length = sizeof(server_bytes), handles = 0;
        if (a20_channel_recv(server, server_bytes, &length, 0, &handles) < 0)
            break;
        if (length == 16 && !a20_memcmp(server_bytes, "BENCH-STATS-V1!!", 16)) {
            uint64_t count = server_calls;
            a20_memcpy(server_bytes, &count, sizeof(count));
            if (serving_network)
                resources();
        } else {
            server_calls++;
        }
        if (a20_channel_send(server, server_bytes, length, 0, 0) < 0) {
            __atomic_store_n(&server_failed, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    a20_thread_exit(0);
}

static int start_echo(void) {
    uint64_t stack;
    if (a20_vm_alloc_pages(16, 3, &stack) < 0)
        return -1;
    a20_thread_create_args_t args = {0};
    args.entry = (uintptr_t)echo;
    args.stack_base = stack + 16 * 4096;
    args.stack_size = 16 * 4096;
    return a20_thread_create(&args) < 0 ? -1 : 0;
}

static void sift(uint64_t *data, unsigned count, unsigned root) {
    for (;;) {
        unsigned child = root * 2 + 1;
        if (child >= count)
            return;
        if (child + 1 < count && data[child] < data[child + 1])
            child++;
        if (data[root] >= data[child])
            return;
        uint64_t value = data[root];
        data[root] = data[child];
        data[child] = value;
        root = child;
    }
}

static void sort(unsigned count) {
    for (unsigned i = count / 2; i; i--)
        sift(samples, count, i - 1);
    for (unsigned end = count; end > 1; end--) {
        uint64_t value = samples[0];
        samples[0] = samples[end - 1];
        samples[end - 1] = value;
        sift(samples, end - 1, 0);
    }
}

static int call(a20_handle_t ep, unsigned bytes, uint64_t sequence) {
    a20_memcpy(request, &sequence, sizeof(sequence));
    uint32_t length = sizeof(reply), handles = 0;
    uint64_t started = now();
    a20_status_t status =
        a20_channel_call(ep, request, bytes, 0, 0, reply, &length, 0, &handles);
    uint64_t finished = now();
    if (status < 0 || finished < started || !started || length != bytes ||
        a20_memcmp(request, reply, bytes)) {
        put("CLUSTER_BENCH: call-error status-magnitude=");
        number(status < 0 ? -status : status);
        put(" bytes=");
        number(bytes);
        put(" received=");
        number(length);
        put(" sequence=");
        number(sequence);
        put("\n");
        resources();
        return -1;
    }
    samples[sequence % BENCH_MAX_CALLS] = finished - started;
    return 0;
}

static int measure(a20_handle_t ep, const char *mode, unsigned bytes,
                   unsigned calls, uint64_t *expected) {
    for (unsigned i = 0; i < bytes; i++)
        request[i] = (uint8_t)(i * 73 + bytes);
    for (unsigned i = 0; i < BENCH_WARMUP; i++)
        if (call(ep, bytes, i) < 0)
            return -1;
    a20_memset(blocks, 0, sizeof(blocks));
    uint64_t total = 0, started = now();
    for (unsigned i = 0; i < calls; i++) {
        if (call(ep, bytes, i) < 0)
            return -1;
        total += samples[i];
        blocks[(uint64_t)i * 100 / calls] += samples[i];
    }
    uint64_t elapsed = now() - started;
    *expected += BENCH_WARMUP + calls;
    uint32_t length = 16, handles = 0;
    uint8_t stats[16];
    if (a20_channel_call(ep, "BENCH-STATS-V1!!", 16, 0, 0, stats, &length, 0,
                         &handles) < 0 ||
        length != 16)
        return -1;
    uint64_t executed;
    a20_memcpy(&executed, stats, sizeof(executed));
    if (executed != *expected)
        return -1;
    sort(calls);
    put("CLUSTER_BENCH: mode=");
    put(mode);
    put(" bytes=");
    number(bytes);
    put(" calls=");
    number(calls);
    put(" sum_ns=");
    number(total);
    put(" elapsed_ns=");
    number(elapsed);
    put(" p50_ns=");
    number(samples[(calls - 1) * 50 / 100]);
    put(" p95_ns=");
    number(samples[(calls - 1) * 95 / 100]);
    put(" p99_ns=");
    number(samples[(calls - 1) * 99 / 100]);
    put(" min_ns=");
    number(samples[0]);
    put(" max_ns=");
    number(samples[calls - 1]);
    put(" executed=");
    number(executed);
    put(" block_sums_ns=");
    for (unsigned i = 0; i < 100; i++) {
        if (i)
            put(",");
        number(blocks[i]);
    }
    put("\n");
    return __atomic_load_n(&server_failed, __ATOMIC_ACQUIRE) ? -1 : 0;
}

static int network(unsigned self, unsigned other, unsigned transport) {
    a20_node_id_t id = {{0}}, peer = {{0}};
    id.bytes[15] = self;
    peer.bytes[15] = other;
    if (a20_cluster_set_self(&id, A20_CLUSTER_CAP_RELIABLE) < 0)
        return -1;
    uint8_t hop[6] = {10, 0, 3, other + 1, 0xab, 0xf4};
    if (transport == A20_CLX_TRANSPORT_LOOPBACK)
        a20_memcpy(hop, &other, 4);
    return a20_cluster_route(A20_ROUTE_ADD, &peer, transport, hop,
                             transport == A20_CLX_TRANSPORT_LOOPBACK ? 4 : 6,
                             0) < 0
               ? -1
               : 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const a20_start_info_t *info = a20_get_start_info();
    if (!info || argc != 3)
        return 1;
    output = info->stdout_handle;
    unsigned calls = 0;
    for (const char *p = argv[2]; *p; p++) {
        if (*p < '0' || *p > '9' || calls > BENCH_MAX_CALLS)
            return fail("count");
        calls = calls * 10 + *p - '0';
    }
    if (calls < 1000 || calls > BENCH_MAX_CALLS || calls % 100)
        return fail("count-range");
    int local = !a20_strcmp(argv[1], "local");
    int loopback = !a20_strcmp(argv[1], "loopback");
    int serving = !a20_strcmp(argv[1], "udp-server");
    serving_network = serving;
    if (!local && !loopback && !serving && a20_strcmp(argv[1], "udp-client"))
        return fail("mode");
    a20_handle_t client = A20_HANDLE_NULL;
    if (local || loopback || serving) {
        a20_channel_pair_t pair = {0};
        if (a20_channel_create(&pair) < 0)
            return fail("pair");
        server = pair.endpoints[1];
        if (local) {
            client = pair.endpoints[0];
        } else {
            if (network(loopback ? 1 : 2, loopback ? 2 : 1,
                        loopback ? A20_CLX_TRANSPORT_LOOPBACK
                                 : A20_CLX_TRANSPORT_UDP) < 0)
                return fail("network-server");
            if (a20_cluster_export(server, "bench", 0, 0) != 1)
                return fail("export");
            a20_hdl_close(pair.endpoints[0]);
        }
        if (start_echo() < 0)
            return fail("thread");
        if (serving) {
            put("CLUSTER_BENCH: SERVER_READY\n");
            uint64_t last = 0;
            for (;;) {
                pause_ms(1000);
                a20_cluster_resource_args_t stats = {0};
                if (a20_cluster_link_resources(&stats) >= 0) {
                    uint64_t errors = stats.values[A20_CLX_LINK_TX_DROPS] +
                                      stats.values[A20_CLX_LINK_RX_DROPS] +
                                      stats.values[A20_CLX_LINK_RX_MALFORMED];
                    if (errors != last) {
                        resources();
                        last = errors;
                    }
                }
            }
        }
    } else if (network(1, 2, A20_CLX_TRANSPORT_UDP) < 0) {
        return fail("network-client");
    }
    if (!local) {
        a20_node_id_t peer = {{0}};
        peer.bytes[15] = 2;
        for (unsigned i = 0; i < 200 && client == A20_HANDLE_NULL; i++) {
            int64_t result = a20_cluster_connect(
                &peer, 1, 0, loopback ? 0 : A20_CONNECT_RELIABLE, 0, &client);
            if (result > 0)
                client = result;
            else
                pause_ms(50);
        }
        if (client == A20_HANDLE_NULL)
            return fail("connect");
    }
    uint64_t expected = 0;
    if (measure(client, argv[1], 64, calls, &expected) < 0 ||
        (!local && !loopback &&
         measure(client, argv[1], BENCH_BYTES, 1000, &expected) < 0))
        return fail("measure");
    a20_hdl_close(client);
    if (!local && !loopback)
        resources();
    put("CLUSTER_BENCH: PASS\n");
    return 0;
}
