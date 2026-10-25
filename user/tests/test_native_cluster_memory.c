/* Two independent kernels; this fixture never supplies memory-service replies. */
#include "liba20rt/a20_cluster_memory.h"
#include "liba20rt/crt0_a20.h"
static a20_handle_t output = A20_HANDLE_NULL;
static const char *ids[] = {"101112131415161718191a1b1c1d1e1f",
                            "202122232425262728292a2b2c2d2e2f"};
static uint8_t page[A20_MEMORY_PAGE], snapshot[A20_MEMORY_PAGE];
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static int fail(const char *why) {
    note("MEMORY_TEST: FAIL ");
    note(why);
    note("\n");
    return 1;
}
static void sleep_ms(uint32_t n) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)n * 1000000});
}
static int all(const uint8_t *p, uint8_t value) {
    for (unsigned i = 0; i < A20_MEMORY_PAGE; i++)
        if (p[i] != value)
            return 0;
    return 1;
}
static int setup(unsigned role) {
    a20_node_id_t self, peer;
    a20_node_id_parse_hex(ids[role], &self);
    a20_node_id_parse_hex(ids[1 - role], &peer);
    if (a20_cluster_set_self(&self, A20_CLUSTER_CAP_RELIABLE) < 0)
        return -1;
    uint8_t address[6] = {10, 0, 3, (uint8_t)(role ? 2 : 3), 0xab, 0xf4};
    if (a20_cluster_route(A20_ROUTE_ADD, &peer, 1, address, 6, 0) < 0)
        return -1;
    a20_runtime_binding_t binding = {.size = 64, .version = 1, .boot_epoch = 1};
    a20_memset(binding.instance_id, 0x55, 16);
    a20_memcpy(binding.domain_id, self.bytes, 16);
    return a20_runtime_bind(&binding);
}
static int spawn_service(const char *path, a20_handle_t *task) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uintptr_t)path,
                                 .path_len = a20_strlen(path),
                                 .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    if (a20_path_open(&file) < 0)
        return -1;
    const char *argv[] = {path, "/bin/memory.conf"};
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .image = file.out_handle,
                                  .root_dir = A20_HANDLE_NULL,
                                  .cwd_dir = A20_HANDLE_NULL,
                                  .event_queue = A20_HANDLE_NULL,
                                  .stdin_handle = A20_HANDLE_NULL,
                                  .stdout_handle = output,
                                  .stderr_handle = output,
                                  .argv = (uintptr_t)argv,
                                  .argc = 2,
                                  .flags = A20_SPAWN_PARENT_BOUND};
    int r = a20_syscall6(A20_SYS_task_spawn, (uintptr_t)&args, 0, 0, 0, 0, 0);
    a20_hdl_close(file.out_handle);
    if (r >= 0)
        *task = args.out_task;
    return r < 0 ? -1 : 0;
}
static int connect_wait(a20_node_id_t *node, uint32_t permission,
                        a20_memory_session_t *s) {
    uint64_t start = a20_agent_clock_ms(0);
    do {
        int r = a20_memory_connect(node->bytes, permission, 2000, s);
        if (r >= 0)
            return 0;
        sleep_ms(20);
    } while (a20_agent_clock_ms(0) - start < 20000);
    return -1;
}
static int write_page(a20_memory_session_t *s, uint32_t index, uint64_t version,
                      uint64_t nonce, uint8_t value,
                      a20_memory_ticket_t *ticket) {
    a20_memset(page, value, sizeof(page));
    uint64_t next;
    return a20_memory_begin(s, index, version, nonce, ticket, 2000) < 0 ||
                   a20_memory_stage_page(s, ticket, page, 3000) < 0 ||
                   a20_memory_finish(s, ticket, A20_MEMORY_COMMIT, &next,
                                     2000) < 0 ||
                   next != version + 1
               ? -1
               : 0;
}
static int stop_service(a20_handle_t task) {
    if (a20_task_kill(task, 15) < 0)
        return -1;
    uint64_t start = a20_agent_clock_ms(0);
    a20_task_status_t status;
    while (a20_agent_clock_ms(0) - start < 5000) {
        int r = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (r >= 0) {
            a20_hdl_close(task);
            return status.exit_code == 0 ? 0 : -1;
        }
        sleep_ms(1);
    }
    return -1;
}
static int server(const char *path, unsigned mode) {
    a20_handle_t task;
    if (spawn_service(path, &task))
        return fail("spawn");
    if (!mode) {
        a20_task_status_t status;
        uint64_t start = a20_agent_clock_ms(0);
        int r;
        do {
            r = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
            sleep_ms(1);
        } while (r == -A20_ERR_WOULD_BLOCK &&
                 a20_agent_clock_ms(0) - start < 5000);
        if (r < 0 || status.exit_code)
            return fail("off-exit");
        a20_hdl_close(task);
        note("MEMORY_TEST: SERVER disabled\n");
        for (;;)
            sleep_ms(100);
    }
    a20_node_id_t local;
    a20_node_id_parse_hex(ids[1], &local);
    a20_memory_session_t s = {.endpoint = A20_HANDLE_NULL};
    if (connect_wait(&local, A20_MEMORY_READ_WRITE, &s))
        return fail("local-connect");
    note("MEMORY_TEST: SERVER ready\n");
    if (mode == 1) {
        for (;;)
            sleep_ms(100);
    }
    uint64_t started = a20_agent_clock_ms(0), version;
    unsigned phase = 0;
    while (a20_agent_clock_ms(0) - started < 40000) {
        int read_status = a20_memory_read_page(&s, 1, snapshot, &version, 2000);
        if (read_status == -A20_ERR_EXISTS) {
            sleep_ms(1);
            continue;
        }
        if (read_status < 0)
            return fail("server-read");
        if (!phase && all(snapshot, 1)) {
            a20_memory_ticket_t t;
            if (write_page(&s, 0, 2, 700, 7, &t))
                return fail("server-write");
            phase = 1;
            note("MEMORY_TEST: SERVER wrote version=3\n");
        } else if (phase == 1 && all(snapshot, 2)) {
            a20_memory_close(&s);
            if (stop_service(task) || spawn_service(path, &task))
                return fail("restart");
            note("MEMORY_TEST: SERVER restarted\n");
            for (;;)
                sleep_ms(100);
        }
        sleep_ms(10);
    }
    return fail("server-timeout");
}
static int client(unsigned mode) {
    a20_node_id_t node;
    a20_node_id_parse_hex(ids[1], &node);
    a20_memory_session_t s = {.endpoint = A20_HANDLE_NULL},
                         ro = {.endpoint = A20_HANDLE_NULL};
    if (a20_memory_connect(node.bytes, A20_MEMORY_OFF, 100, &s) !=
        -A20_ERR_NOT_SUPPORTED)
        return fail("client-off");
    if (!mode) {
        // A live peer directory must explicitly report no memory export.
        uint64_t started = a20_agent_clock_ms(0), epoch;
        a20_runtime_info_t peer;
        while (a20_cluster_runtime_query(&node, 1000, &peer, &epoch) < 0) {
            if (a20_agent_clock_ms(0) - started > 15000)
                return fail("off-peer");
            sleep_ms(20);
        }
        sleep_ms(500);
        uint32_t slot;
        if (a20_cluster_resolve(&node, A20_MEMORY_SERVICE, 14, 2000, &slot,
                                &epoch) != -A20_ERR_NOT_FOUND)
            return fail("off-export");
        note("MEMORY_TEST: PASS disabled no-export\n");
        return 0;
    }
    if (connect_wait(&node, A20_MEMORY_READ_WRITE, &s))
        return fail("connect");
    uint64_t version = 99;
    if (a20_memory_read_page(&s, 0, snapshot, &version, 3000) < 0 ||
        version != 1 || !all(snapshot, mode == 1 ? 13 : 0))
        return fail("initial-snapshot");
    a20_memory_ticket_t t;
    if (mode == 1) {
        if (s.config.mode != A20_MEMORY_READ_ONLY ||
            a20_memory_begin(&s, 0, 1, 1, &t, 1000) != -A20_ERR_ACCESS)
            return fail("readonly");
        if (a20_memory_read_page(&s, 1, snapshot, &version, 3000) < 0 ||
            version != 1)
            return fail("seed-tail");
        for (unsigned i = 0; i < A20_MEMORY_PAGE; i++)
            if (snapshot[i] != (i < 512 ? 29 : 0))
                return fail("seed-padding");
        a20_memory_close(&s);
        note("MEMORY_TEST: PASS read-only\n");
        return 0;
    }
    if (connect_wait(&node, A20_MEMORY_READ_ONLY, &ro) ||
        a20_memory_begin(&ro, 0, 1, 1, &t, 1000) != -A20_ERR_ACCESS)
        return fail("client-readonly");
    a20_memory_close(&ro);
    if (a20_memory_begin(&s, 0, 1, 10, &t, 2000) < 0)
        return fail("begin");
    a20_memory_ticket_t contender;
    if (a20_memory_begin(&s, 1, 1, 11, &contender, 2000) !=
        -A20_ERR_WOULD_BLOCK)
        return fail("writer-exclusion");
    a20_memset(page, 42, sizeof(page));
    if (a20_memory_stage_page(&s, &t, page, 3000) < 0 ||
        a20_memory_read_page(&s, 0, snapshot, &version, 3000) < 0 ||
        !all(snapshot, 0) || version != 1)
        return fail("staging-visibility");
    if (a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 2000) < 0 ||
        version != 2 ||
        a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 2000) < 0 ||
        version != 2)
        return fail("commit-replay");
    if (a20_memory_read_page(&s, 0, snapshot, &version, 3000) < 0 ||
        !all(snapshot, 42))
        return fail("committed-snapshot");
    if (write_page(&s, 1, 1, 20, 1, &t))
        return fail("signal-server");
    uint64_t started = a20_agent_clock_ms(0);
    do {
        int read_status = a20_memory_read_page(&s, 0, page, &version, 3000);
        if (read_status == -A20_ERR_EXISTS) {
            sleep_ms(1);
            continue;
        }
        if (read_status < 0)
            return fail("refresh");
        if (a20_agent_clock_ms(0) - started > 10000)
            return fail("refresh-timeout");
        if (version != 3)
            sleep_ms(10);
    } while (version != 3);
    if (!all(page, 7) || !all(snapshot, 42))
        return fail("snapshot-is-private");
    if (a20_memory_begin(&s, 0, 2, 21, &t, 1000) != -A20_ERR_EXISTS)
        return fail("version-conflict");
    if (a20_memory_begin(&s, 0, 3, 22, &t, 1000) < 0)
        return fail("expiry-begin");
    a20_memset(page, 9, sizeof(page));
    if (a20_memory_stage_page(&s, &t, page, 3000) < 0)
        return fail("expiry-stage");
    sleep_ms(s.config.transaction_ms + 50);
    if (a20_memory_finish(&s, &t, A20_MEMORY_COMMIT, &version, 1000) !=
            -A20_ERR_EXPIRED ||
        a20_memory_read_page(&s, 0, page, &version, 3000) < 0 || version != 3 ||
        !all(page, 7))
        return fail("expired-write");
    if (a20_memory_begin(&s, 0, 3, 23, &t, 1000) < 0 ||
        a20_memory_stage_page(&s, &t, page, 3000) < 0 ||
        a20_memory_finish(&s, &t, A20_MEMORY_ABORT, 0, 1000) < 0)
        return fail("abort");
    a20_memory_ticket_t old = t;
    if (write_page(&s, 1, 2, 30, 2, &t))
        return fail("restart-signal");
    sleep_ms(500);
    a20_memory_close(&s);
    if (connect_wait(&node, A20_MEMORY_READ_WRITE, &s) ||
        a20_memory_finish(&s, &old, A20_MEMORY_COMMIT, &version, 1000) !=
            -A20_ERR_EXPIRED ||
        a20_memory_read_page(&s, 0, page, &version, 3000) < 0 || version != 1 ||
        !all(page, 0))
        return fail("restart-fencing");
    a20_memory_close(&s);
    note("MEMORY_TEST: PASS remote pages bidirectional atomic replay expiry "
         "abort restart\n");
    return 0;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    if (argc != 4 || (argv[1][0] != '0' && argv[1][0] != '1') || argv[1][1] ||
        argv[2][0] < '0' || argv[2][0] > '2' || argv[2][1])
        return fail("arguments");
    unsigned role = argv[1][0] - '0', mode = argv[2][0] - '0';
    if (setup(role) < 0)
        return fail("setup");
    return role ? server(argv[3], mode) : client(mode);
}
