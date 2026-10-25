/* Optional remote page service. Transport/lifecycle in C; state in Rust. */
#include "liba20rt/a20_cluster_agent.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_memory.h"
static union {
    uint64_t alignment;
    uint8_t bytes[A20_MEMORY_STORAGE];
} owner;
static a20_handle_t output = A20_HANDLE_NULL;
static char initial_file[A20_CONFIG_PATH];
int memcmp(const void *a, const void *b, size_t n) {
    return a20_memcmp(a, b, n);
}
int bcmp(const void *a, const void *b, size_t n) {
    return a20_memcmp(a, b, n);
}
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
void a20_memory_panic(void) {
    note("CLUSTER_MEMORY: Rust invariant failure\n");
    a20_task_exit(70);
    for (;;) {
    }
}
static int load(const char *path, a20_memory_config_t *config) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uintptr_t)path,
                                 .path_len = a20_strlen(path),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return -1;
    static char text[A20_CONFIG_BYTES + 1];
    uint32_t used = 0;
    int result = -1;
    while (used < sizeof(text)) {
        uint64_t got = 0;
        if (a20_hdl_read_buf(file.out_handle, text + used, sizeof(text) - used,
                             &got) < 0 ||
            got > sizeof(text) - used)
            break;
        if (!got) {
            result = a20_memory_config_parse_file(text, used, config,
                                                  initial_file, 0);
            break;
        }
        used += (uint32_t)got;
    }
    a20_hdl_close(file.out_handle);
    return result;
}
static int seed_file(uint32_t pages) {
    if (!initial_file[0])
        return 0;
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uintptr_t)initial_file,
                                 .path_len = a20_strlen(initial_file),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return -1;
    static uint8_t bytes[A20_MEMORY_PAGE];
    uint32_t index = 0, used = 0;
    int result = -1;
    for (;;) {
        uint64_t got = 0;
        if (a20_hdl_read_buf(file.out_handle, bytes + used,
                             sizeof(bytes) - used, &got) < 0 ||
            got > sizeof(bytes) - used)
            break;
        if (!got) {
            result =
                !used || (index < pages && !a20_memory_owner_seed(
                                               owner.bytes, index, bytes, used))
                    ? 0
                    : -1;
            break;
        }
        used += (uint32_t)got;
        if (used == sizeof(bytes)) {
            if (index >= pages ||
                a20_memory_owner_seed(owner.bytes, index++, bytes, used))
                break;
            used = 0;
        }
    }
    a20_hdl_close(file.out_handle);
    return result;
}
static int dispatch(void *ctx, const uint8_t *q, uint32_t len, uint8_t *reply,
                    uint32_t capacity) {
    return a20_memory_owner_dispatch(ctx, q, len, reply, capacity,
                                     a20_agent_clock_ms(0));
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    a20_memory_config_t config;
    if (argc != 2 || load(argv[1], &config)) {
        note("CLUSTER_MEMORY: invalid configuration\n");
        return 2;
    }
    // OFF needs no runtime binding, channel, export, timer or network pump.
    if (config.mode == A20_MEMORY_OFF) {
        note("CLUSTER_MEMORY: disabled\n");
        return 0;
    }
    a20_runtime_info_t runtime;
    a20_cluster_snapshot_args_t self;
    a20_handle_identity_t identity;
    uint8_t encoded[64];
    if (!si || a20_runtime_info(&runtime) < 0 ||
        runtime.state != A20_RUNTIME_BOUND || a20_cluster_snapshot(&self) < 0 ||
        !a20_runtime_id_equal(runtime.domain_id, self.node_id) ||
        a20_hdl_identity(si->self_task, &identity) < 0 ||
        a20_object_id_encode(encoded, sizeof(encoded), &identity.id) ||
        a20_memory_owner_init(owner.bytes, sizeof(owner.bytes), &config,
                              encoded)) {
        note("CLUSTER_MEMORY: identity or owner initialization failed\n");
        return 3;
    }
    if (seed_file(config.pages)) {
        note("CLUSTER_MEMORY: initial file exceeds capacity or cannot be "
             "read\n");
        return 6;
    }
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0)
        return 4;
    if (a20_cluster_export(pair.endpoints[0], A20_MEMORY_SERVICE, 0, 0) < 0) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        return 4;
    }
    a20_hdl_close(pair.endpoints[1]);
    a20_cluster_agent_server_t server;
    a20_work_agent_t unused = {0};
    a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
    server.dispatch = dispatch;
    server.dispatch_ctx = owner.bytes;
    note("CLUSTER_MEMORY: ready\n");
    int result = 0;
    while (!(a20_signal_check() & (1ULL << 15))) {
        int handled = a20_cluster_agent_pump(&server);
        if (handled < 0) {
            result = 5;
            break;
        }
        if (!handled)
            a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    for (unsigned i = 0; i < A20_AGENT_LOCAL_CONNECTIONS; i++)
        if (server.local[i] != A20_HANDLE_NULL)
            a20_hdl_close(server.local[i]);
    if (server.exported != A20_HANDLE_NULL)
        a20_hdl_close(server.exported);
    note("CLUSTER_MEMORY: stopped\n");
    return result;
}
