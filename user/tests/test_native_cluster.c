/*
 * Cluster ABI contract tests (docs/cluster/01-abi.md, stage W0).
 *
 * The kernel cluster data plane has landed in WA1, so this test pins the
 * implemented identity behavior alongside the frozen syscall surface:
 *
 *   abi-nr    every cluster syscall number is the documented value and is
 *             reachable through the dispatch table (a wrong number returns
 *             ENOSYS/NOT_SUPPORTED from the generic unknown-syscall path, not
 *             the cluster refusal)
 *   abi-live  set_self succeeds once and rejects a second identity with
 *             A20_ERR_EXISTS
 *   abi-args  each rejects the argument shapes the ABI says are illegal, and
 *             does so *before* the refusal, so a malformed caller never sees
 *             the stub's answer
 *   abi-node  node id hashing matches the FNV-1a vector in the spec, since the
 *             same value has to appear in a frame header on the wire
 *
 * The abi-args partition is the one that matters going forward: when the real
 * handler lands in a later stage, these cases must keep failing the same way.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_cluster_agent.h"
#include "liba20rt/a20_cluster_client.h"
#include "liba20rt/a20_cluster_job.h"

static a20_handle_t g_stdout = A20_HANDLE_NULL;

static int fail(const char *what) {
    if (g_stdout != A20_HANDLE_NULL) {
        char buf[128];
        int n = 0;
        buf[n++] = 'F';
        buf[n++] = ':';
        const char *p = what;
        while (*p && n < (int)sizeof(buf) - 2)
            buf[n++] = *p++;
        buf[n++] = '\n';
        a20_hdl_write_buf(g_stdout, buf, (uint64_t)n, NULL);
    }
    return 1;
}

static void note(const char *msg, uint64_t len) {
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, msg, len, NULL);
}

static void note_value(const char *label, uint64_t value) {
    note(label, a20_strlen(label));
    char text[17];
    for (unsigned i = 0; i < 16; i++)
        text[i] = "0123456789abcdef"[(value >> ((15 - i) * 4)) & 15];
    text[16] = '\n';
    note(text, sizeof(text));
}

static int spawn_service(const char *path, a20_handle_t *task) {
    a20_path_open_args_t image = {.dir = A20_HANDLE_NULL,
                                  .rights = A20_RIGHT_READ | A20_RIGHT_EXEC,
                                  .path = (uint64_t)(uintptr_t)path,
                                  .path_len = (uint32_t)a20_strlen(path)};
    if (a20_path_open(&image) < 0)
        return -1;
    const char *argv[] = {path, 0};
    a20_task_spawn_args_t spawn = {.size = sizeof(spawn),
                                   .version = 2,
                                   .image = image.out_handle,
                                   .root_dir = A20_HANDLE_NULL,
                                   .cwd_dir = A20_HANDLE_NULL,
                                   .event_queue = A20_HANDLE_NULL,
                                   .stdin_handle = A20_HANDLE_NULL,
                                   .stdout_handle = g_stdout,
                                   .stderr_handle = g_stdout,
                                   .argv = (uint64_t)(uintptr_t)argv,
                                   .argc = 1};
    a20_status_t r =
        a20_syscall6(A20_SYS_task_spawn, (uint64_t)&spawn, 0, 0, 0, 0, 0);
    a20_hdl_close(image.out_handle);
    if (r < 0)
        return -1;
    *task = spawn.out_task;
    return 0;
}

static void stop_service(a20_handle_t task) {
    if (task == A20_HANDLE_NULL)
        return;
    a20_task_kill(task, 9);
    a20_task_status_t status;
    uint64_t started = a20_agent_clock_ms(0);
    while (a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status) ==
               -A20_ERR_WOULD_BLOCK &&
           a20_agent_clock_ms(0) - started < 1000)
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    a20_hdl_close(task);
}

static int abi_numbers(void) {
    if (A20_SYS_cluster_set_self != 0x0520)
        return fail("nr-set_self");
    if (A20_SYS_cluster_export != 0x0521)
        return fail("nr-export");
    if (A20_SYS_cluster_connect != 0x0522)
        return fail("nr-connect");
    if (A20_SYS_cluster_route != 0x0523)
        return fail("nr-route");
    if (A20_SYS_cluster_event_subscribe != 0x0524)
        return fail("nr-events");
    if (A20_SYS_cluster_link_status != 0x0525)
        return fail("nr-link");
    if (A20_SYS_cluster_snapshot != 0x0526)
        return fail("nr-snapshot");
    return 0;
}

static int abi_node_hash(void) {
    a20_node_id_t id;

    a20_node_id_local(&id);
    if (!a20_node_id_is_local(&id))
        return fail("node-local");

    /* FNV-1a (offset 2166136261, prime 16777619) over the 16 id bytes. These
     * two vectors are the values the frame header carries, so a change to the
     * recurrence on either side of the wire shows up as a peer mismatch. */
    if (a20_node_hash(&id) != 0x69691905u)
        return fail("node-hash-zero");

    a20_node_id_broadcast(&id);
    if (a20_node_hash(&id) != 0x360779f5u)
        return fail("node-hash-ff");

    a20_node_id_broadcast(&id);
    if (a20_node_id_is_local(&id))
        return fail("node-bcast-not-local");
    return 0;
}

/* A well-formed peer identity: neither reserved value. */
static void peer_id(a20_node_id_t *out) {
    uint8_t raw[16];
    for (int i = 0; i < 16; i++)
        raw[i] = (uint8_t)(0x10 + i);
    a20_node_id_set_raw(out, raw);
}

static int abi_live_identity(void) {
    a20_node_id_t peer;
    a20_node_id_t routed;
    peer_id(&peer);

    a20_status_t r = a20_cluster_set_self(&peer, A20_CLUSTER_CAP_RELIABLE);
    if (r != 0)
        return fail("set-self-first");
    if (a20_cluster_set_self(&peer, A20_CLUSTER_CAP_RELIABLE) !=
        -(a20_status_t)A20_ERR_EXISTS)
        return fail("set-self-second");

    /* Exercise the committed local export/name lookup path.  The server
     * endpoint is owned by this task and is closed on exit; the temporary
     * caller endpoint is closed here. */
    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create(&pair)))
        return fail("live-channel-create");
    uint32_t slot = 0;
    r = a20_cluster_export(pair.endpoints[0], "native-cluster", 0, &slot);
    if (r < 0 || slot == 0)
        return fail("live-export");
    if (a20_cluster_export(pair.endpoints[0], "native-cluster", 0, NULL) !=
        -A20_ERR_EXISTS)
        return fail("live-export-duplicate-keeps-handle-reference");
    /* Registration pins the peer; it does not steal its installed handle's
     * reference. Close the unused user peer while the export stays live. */
    if (a20_hdl_close(pair.endpoints[1]) < 0)
        return fail("live-close-export-peer-handle");
    a20_handle_t caller = A20_HANDLE_NULL;
    r = a20_cluster_connect(NULL, slot, NULL, 0, 0, &caller);
    if (r < 0 || caller == A20_HANDLE_NULL)
        return fail("live-connect-local");
    if (a20_hdl_close(caller) < 0)
        return fail("live-close-caller");
    if (a20_hdl_close(pair.endpoints[0]) < 0)
        return fail("live-close-server-export");
    a20_thread_yield();

    /* Route-table lookup and loopback link status are observable through the
     * public ABI; the synthetic next-hop is a 32-bit loopback node number. */
    peer_id(&routed);
    routed.bytes[0]++;
    uint32_t vnode = 7;
    if (a20_cluster_route(A20_ROUTE_REPLACE, &routed,
                          A20_CLX_TRANSPORT_LOOPBACK, &vnode, sizeof(vnode),
                          0) != 0)
        return fail("live-route-replace");
    a20_cluster_link_status_args_t link;
    if (a20_cluster_link_status(&routed, &link) != 0 ||
        link.state != A20_CLX_LINK_UP)
        return fail("live-loopback-status");
    if (a20_cluster_route(A20_ROUTE_DEL, &routed, A20_CLX_TRANSPORT_LOOPBACK,
                          NULL, 0, 0) != 0)
        return fail("live-route-delete");

    return 0;
}

static int abi_snapshot_and_agent(void) {
    a20_cluster_snapshot_args_t self = {0};
    if (a20_cluster_snapshot(&self) < 0 || !self.boot_epoch ||
        self.profile != 2 || self.max_inflight == 0)
        return fail("snapshot-self");
    a20_cluster_resource_args_t resources;
    a20_memset(&resources, 0xff, sizeof(resources));
    if (a20_cluster_resources(&resources) < 0 ||
        resources.values[A20_CLX_STAT_BOOT_EPOCH] != self.boot_epoch ||
        resources.values[A20_CLX_STAT_MAX_ROUTES] != self.max_routes ||
        resources.values[A20_CLX_STAT_MAX_EXPORTS] != self.max_exports ||
        resources.values[A20_CLX_STAT_MAX_PROXIES] != self.max_remote_eps ||
        resources.values[A20_CLX_STAT_REPLAY_BYTES] >
            resources.values[A20_CLX_STAT_MAX_REPLAY_BYTES])
        return fail("snapshot-resources");
    a20_node_id_t id;
    peer_id(&id);
    if (a20_memcmp(self.node_id, id.bytes, 16))
        return fail("snapshot-self-identity");
    uint32_t nh = 19;
    id.bytes[0]++;
    if (a20_cluster_route(A20_ROUTE_ADD, &id, A20_CLX_TRANSPORT_LOOPBACK, &nh,
                          4, 5) < 0)
        return fail("snapshot-route-add");
    a20_cluster_snapshot_args_t route = {.kind = A20_CLUSTER_SNAPSHOT_ROUTE};
    if (a20_cluster_snapshot(&route) < 0 || route.metric != 5 ||
        a20_memcmp(route.node_id, id.bytes, 16))
        return fail("snapshot-route-record");
    if (a20_cluster_route(A20_ROUTE_REPLACE, &id, A20_CLX_TRANSPORT_LOOPBACK,
                          &nh, 4, 6) < 0 ||
        a20_cluster_snapshot(&route) != -A20_ERR_BUSY)
        return fail("snapshot-route-generation");
    a20_cluster_route(A20_ROUTE_DEL, &id, A20_CLX_TRANSPORT_LOOPBACK, 0, 0, 0);
    self.kind = 99;
    if (a20_cluster_snapshot(&self) != -A20_ERR_INVALID_ARGUMENT)
        return fail("snapshot-invalid-kind");
    a20_path_open_args_t image = {0};
    image.dir = A20_HANDLE_NULL;
    const char path[] = "/bin/cluster-agent-rv";
    image.path = (uint64_t)(uintptr_t)path;
    image.path_len = sizeof(path) - 1;
    image.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    if (a20_path_open(&image) < 0)
        return fail("agent-open-image");
    const char *argv[] = {path, "101112131415161718191a1b1c1d1e1f",
                          "/bin/cluster-task-probe-rv", 0};
    a20_task_spawn_args_t spawn = {0};
    spawn.root_dir = spawn.cwd_dir = spawn.event_queue = A20_HANDLE_NULL;
    spawn.stdin_handle = A20_HANDLE_NULL;
    spawn.stdout_handle = spawn.stderr_handle = g_stdout;
    spawn.image = image.out_handle;
    spawn.argv = (uint64_t)(uintptr_t)argv;
    spawn.argc = 3;
    spawn.size = sizeof(spawn);
    spawn.version = 2;
    if (a20_syscall6(A20_SYS_task_spawn, (uint64_t)&spawn, 0, 0, 0, 0, 0) < 0)
        return fail("agent-spawn");
    a20_hdl_close(image.out_handle);
    a20_handle_t client = A20_HANDLE_NULL;
    uint64_t started = a20_agent_clock_ms(0);
    while (a20_agent_clock_ms(0) - started < 2000) {
        if (a20_cluster_connect(0, 0, "work", 0, 0, &client) >= 0)
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    int ok = client != A20_HANDLE_NULL;
    if (!ok)
        note("agent-connect timed out\n", 24);
    uint8_t input[128], reply[128], data[12];
    a20_work_put32(data, 1);
    a20_work_put32(data + 4, (uint32_t)-7);
    a20_work_put32(data + 8, 6);
    for (unsigned kind = 1; ok && kind <= 3; kind++) {
        a20_work_request_t request = {
            .kind = kind,
            .job = 123,
            .shard = 4,
            .operator_id = kind == A20_WORK_QUERY ? 0 : 1,
            .budget_ms = 1000,
            .input = data,
            .input_len = kind == A20_WORK_OPERATOR ? 12 : 0,
            .attempt = 7};
        int n = a20_work_encode(input, sizeof(input), &request);
        uint32_t got = sizeof(reply);
        ok = a20_channel_send(client, input, n, 0, 0) == 0;
        started = a20_agent_clock_ms(0);
        a20_status_t r = -A20_ERR_WOULD_BLOCK;
        while (ok && r == -A20_ERR_WOULD_BLOCK &&
               a20_agent_clock_ms(0) - started < 2000) {
            got = sizeof(reply);
            uint32_t handle_count = 0;
            r = a20_channel_recv_flags(client, reply, &got, 0, &handle_count,
                                       A20_MSG_NONBLOCK);
            if (r == -A20_ERR_WOULD_BLOCK)
                a20_thread_sleep((a20_time_t){.nsecs = 1000000});
        }
        a20_work_result_t result;
        ok = ok && r >= 0 &&
             a20_work_result_decode(reply, got, &request, &result) == 0 &&
             result.status == 0;
        if (ok && kind == A20_WORK_QUERY)
            ok = result.output_len == 56 &&
                 a20_work_u32(result.output) ==
                     (A20_WORK_CAP_OPERATOR | A20_WORK_CAP_PROCESS);
        if (ok && kind == A20_WORK_OPERATOR)
            ok = result.output_len == 8 &&
                 (int64_t)a20_work_u64(result.output) == -42;
        if (ok && kind == A20_WORK_PROCESS)
            ok = result.output_len == 8 && a20_work_u32(result.output) == 42;
        note_value("agent-kind=", kind);
        if (!ok) {
            note_value("agent-recv=", (uint64_t)r);
            note_value("agent-length=", got);
            if (r >= 0 && got >= 32)
                note_value("agent-status=", a20_work_u32(reply + 20));
        }
    }
    if (client != A20_HANDLE_NULL)
        a20_hdl_close(client);
    note(ok ? "agent-calls ok\n" : "agent-calls failed\n", ok ? 15 : 19);
    a20_cluster_snapshot_args_t old_export = {.kind =
                                                  A20_CLUSTER_SNAPSHOT_EXPORT};
    int have_old_export = a20_cluster_snapshot(&old_export) >= 0;
    a20_task_kill(spawn.out_task, 9);
    a20_task_status_t status;
    a20_task_wait(spawn.out_task, 0, &status);
    a20_hdl_close(spawn.out_task);
    if (!ok)
        return fail("agent-real-work-or-process");
    /* Restart only the user agent, keeping identity and data-plane state.
     * Its process image now deliberately runs until the work deadline. */
    argv[2] = "/bin/cluster-hang-probe-rv";
    if (a20_path_open(&image) < 0)
        return fail("agent-restart-open");
    spawn.image = image.out_handle;
    if (a20_syscall6(A20_SYS_task_spawn, (uint64_t)&spawn, 0, 0, 0, 0, 0) < 0)
        return fail("agent-restart-spawn");
    a20_hdl_close(image.out_handle);
    client = A20_HANDLE_NULL;
    started = a20_agent_clock_ms(0);
    while (a20_agent_clock_ms(0) - started < 2000) {
        a20_cluster_snapshot_args_t current = {.kind =
                                                   A20_CLUSTER_SNAPSHOT_EXPORT};
        if (a20_cluster_snapshot(&current) >= 0 && have_old_export &&
            current.slot > old_export.slot &&
            a20_cluster_connect(0, 0, "work", 0, 0, &client) >= 0)
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    ok = client != A20_HANDLE_NULL;
    for (unsigned step = 0; ok && step < 2; step++) {
        a20_work_request_t request = {.kind = step ? A20_WORK_QUERY
                                                   : A20_WORK_PROCESS,
                                      .job = 456,
                                      .operator_id = step ? 0 : 1,
                                      .budget_ms = 100,
                                      .attempt = 2};
        int n = a20_work_encode(input, sizeof(input), &request);
        ok = a20_channel_send(client, input, n, 0, 0) >= 0;
        started = a20_agent_clock_ms(0);
        a20_status_t r = -A20_ERR_WOULD_BLOCK;
        uint32_t got = sizeof(reply);
        while (ok && r == -A20_ERR_WOULD_BLOCK &&
               a20_agent_clock_ms(0) - started < 2000) {
            uint32_t handles = 0;
            got = sizeof(reply);
            r = a20_channel_recv_flags(client, reply, &got, 0, &handles,
                                       A20_MSG_NONBLOCK);
            if (r == -A20_ERR_WOULD_BLOCK)
                a20_thread_sleep((a20_time_t){.nsecs = 1000000});
        }
        a20_work_result_t result;
        ok = ok && r >= 0 &&
             a20_work_result_decode(reply, got, &request, &result) == 0 &&
             result.status == (step ? A20_WORK_OK : A20_WORK_DEADLINE);
    }
    a20_cluster_snapshot_args_t recovered = {0};
    ok = ok && a20_cluster_snapshot(&recovered) >= 0 &&
         recovered.boot_epoch == self.boot_epoch;
    if (client != A20_HANDLE_NULL)
        a20_hdl_close(client);
    a20_task_kill(spawn.out_task, 9);
    a20_task_wait(spawn.out_task, 0, &status);
    a20_hdl_close(spawn.out_task);
    if (ok)
        note("agent-deadline-restart ok\n", 26);
    return ok ? 0 : fail("agent-real-work-or-process");
}

static int work_dot_client(a20_handle_t ep) {
    uint8_t data[12], input[64], reply[64];
    a20_work_put32(data, 1);
    a20_work_put32(data + 4, (uint32_t)-7);
    a20_work_put32(data + 8, 6);
    a20_work_request_t request = {.kind = A20_WORK_OPERATOR,
                                  .job = 555,
                                  .shard = 3,
                                  .operator_id = A20_WORK_DOT,
                                  .budget_ms = 500,
                                  .input_len = sizeof(data),
                                  .input = data};
    int n = a20_work_encode(input, sizeof(input), &request);
    uint32_t len = sizeof(reply);
    a20_work_result_t result;
    return a20_cluster_exchange(ep, input, n, reply, &len, 1000) >= 0 &&
           a20_work_result_decode(reply, len, &request, &result) == 0 &&
           result.status == 0 && result.output_len == 8 &&
           (int64_t)a20_work_u64(result.output) == -42;
}

static int abi_clusterd(void) {
    /* A live unrelated service intentionally never answers CW requests. */
    a20_channel_pair_t unrelated;
    if (a20_channel_create(&unrelated) < 0 ||
        a20_cluster_export(unrelated.endpoints[0], "unresponsive", 0, 0) < 0)
        return fail("clusterd-unrelated-export");
    a20_hdl_close(unrelated.endpoints[1]);
    a20_handle_t agent = A20_HANDLE_NULL, daemon = A20_HANDLE_NULL,
                 client = A20_HANDLE_NULL;
    int ok = spawn_service("/bin/cluster-agent-rv", &agent) == 0 &&
             spawn_service("/bin/clusterd-rv", &daemon) == 0;
    uint64_t started = a20_agent_clock_ms(0);
    a20_cluster_selection_t target;
    while (ok && a20_agent_clock_ms(0) - started < 4000) {
        if (a20_cluster_connect_uri("service://any/work", A20_WORK_OPERATOR, 1,
                                    12, 300, &client, &target) >= 0)
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 10000000});
    }
    ok = ok && client != A20_HANDLE_NULL && work_dot_client(client) &&
         (target.flags & A20_WORK_REPLAY_SAFE) && target.max_input == 1024;
    if (ok)
        ok = a20_cluster_resolve_any("work", A20_WORK_PROCESS, 1, 0, 300,
                                     &target) == -A20_ERR_NOT_FOUND &&
             a20_cluster_resolve_any("work", A20_WORK_OPERATOR, 1, 2000, 300,
                                     &target) == -A20_ERR_NOT_FOUND;
    stop_service(daemon);
    daemon = A20_HANDLE_NULL;
    /* Control-plane replacement must preserve an established service channel. */
    ok = ok && work_dot_client(client);
    if (client != A20_HANDLE_NULL)
        a20_hdl_close(client);
    client = A20_HANDLE_NULL;
    ok = ok && spawn_service("/bin/clusterd-rv", &daemon) == 0;
    started = a20_agent_clock_ms(0);
    while (ok && a20_agent_clock_ms(0) - started < 4000) {
        if (a20_cluster_connect_uri("service://any/work", A20_WORK_OPERATOR, 1,
                                    12, 300, &client, &target) >= 0)
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 10000000});
    }
    ok = ok && client != A20_HANDLE_NULL && work_dot_client(client);
    if (client != A20_HANDLE_NULL)
        a20_hdl_close(client);
    if (ok) {
        a20_node_id_t local = {{0}};
        a20_handle_t stale = A20_HANDLE_NULL;
        ok = a20_cluster_connect_bound(&local, target.slot,
                                       target.boot_epoch ^ 1,
                                       &stale) == -A20_ERR_NODE_UNREACHABLE;
    }
    a20_handle_t coordinator = A20_HANDLE_NULL, jobs = A20_HANDLE_NULL;
    ok = ok && spawn_service("/bin/jobd-rv", &coordinator) == 0;
    started = a20_agent_clock_ms(0);
    while (ok && a20_agent_clock_ms(0) - started < 2000) {
        if (a20_cluster_connect(0, 0, "cluster.jobs", 0, 0, &jobs) >= 0)
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    if (ok && jobs != A20_HANDLE_NULL) {
        uint8_t wire[1056] = {0}, response[40];
        wire[0] = 'C';
        wire[1] = 'J';
        wire[2] = 1;
        wire[3] = A20_JOB_DOT;
        a20_work_put64(wire + 4, 800);
        a20_work_put32(wire + 12, 5000);
        a20_work_put32(wire + 16, 128);
        uint64_t expected = 0;
        for (unsigned i = 0; i < 128; i++) {
            a20_work_put32(wire + 32 + i * 4, (uint32_t)(int32_t)((int)i - 64));
            a20_work_put32(wire + 32 + (128 + i) * 4, i);
            expected += (uint64_t)((int64_t)((int)i - 64) * i);
        }
        uint32_t len = sizeof(response);
        ok = a20_cluster_exchange(jobs, wire, sizeof(wire), response, &len,
                                  6000) >= 0 &&
             len == 40 && response[0] == 'C' && response[1] == 'J' &&
             a20_work_u64(response + 4) == 800 &&
             !a20_work_u32(response + 12) &&
             a20_work_u32(response + 16) == 15 &&
             a20_work_u32(response + 24) == 15 &&
             a20_work_u64(response + 32) == expected;
        if (ok)
            note("jobd-binary-tree ok\n", 20);
        a20_hdl_close(jobs);
    } else {
        ok = 0;
    }
    stop_service(coordinator);
    stop_service(agent);
    agent = A20_HANDLE_NULL;
    started = a20_agent_clock_ms(0);
    int removed = 0;
    while (ok && a20_agent_clock_ms(0) - started < 3000) {
        if (a20_cluster_resolve_any("work", A20_WORK_OPERATOR, 1, 12, 300,
                                    &target) == -A20_ERR_NOT_FOUND) {
            removed = 1;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 10000000});
    }
    stop_service(daemon);
    a20_hdl_close(unrelated.endpoints[0]);
    if (ok && removed) {
        note("clusterd-any-recovery ok\n", 24);
        return 0;
    }
    return fail("clusterd-any-recovery");
}

/* Argument validation runs ahead of the refusal: each case below is illegal by
 * 01-abi.md and must come back as INVALID_ARGUMENT, never as the stub's
 * UNSUPPORTED, so the rule survives the data plane landing. */
static int abi_arg_rejection(void) {
    a20_node_id_t id;
    a20_node_id_local(&id);
    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create(&pair)))
        return fail("args-channel-create");

    /* set_self with a reserved node id (all-zero LOCAL, all-0xff BROADCAST). */
    if (a20_cluster_set_self(&id, A20_CLUSTER_CAP_RELIABLE) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-local");
    a20_node_id_broadcast(&id);
    if (a20_cluster_set_self(&id, 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-bcast");
    if (a20_cluster_set_self(NULL, 0x8u) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-set_self-caps");

    /* export with a null channel and an over-long name. */
    char longname[A20_CLUSTER_SERVICE_NAME_MAX + 2];
    for (unsigned i = 0; i < sizeof(longname); i++)
        longname[i] = 'x';
    if (a20_cluster_export(pair.endpoints[0], longname, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-export-name");

    /* export with an unknown flag bit. */
    if (a20_cluster_export(pair.endpoints[0], "svc", 0x80u, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-export-flags");

    /* connect with an unknown flag bit. */
    if (a20_cluster_connect(NULL, 1, "svc", 0x80u, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-connect-flags");

    /* route with an out-of-range op and an over-long next hop. */
    a20_node_id_local(&id);
    uint8_t hop[32];
    for (unsigned i = 0; i < sizeof(hop); i++)
        hop[i] = 0;
    if (a20_cluster_route(99u, &id, A20_CLX_TRANSPORT_LOOPBACK, hop,
                          sizeof(hop), 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-route-op");
    if (a20_cluster_route(A20_ROUTE_ADD, &id, A20_CLX_TRANSPORT_LOOPBACK, hop,
                          sizeof(hop), 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-route-hop-len");

    /* event_subscribe with bits outside the documented mask. */
    if (a20_cluster_event_subscribe(0x10u, 0) != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-event-mask");

    return 0;
}

/* A struct that is too short for the fields the kernel knows must be refused
 * before anything else is read (docs/native-abi/01-types.md §2). */
static int abi_struct_versioning(void) {
    a20_cluster_connect_args_t args;
    args.size = sizeof(args) - 8; /* one pointer short */
    args.version = 1;
    args.flags = 0;
    args.slot = 0;
    args.timeout_ms = 0;
    args.reserved = 0;
    a20_node_id_local(&args.node_id);
    args.service_name = 0;
    args.name_len = 0;

    a20_status_t r =
        a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-short-struct");

    args.version = 0; /* version 0 is never valid */
    r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-version-zero");

    args.size = sizeof(args);
    args.version = 99; /* newer than the kernel knows */
    r = a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_INVALID_ARGUMENT)
        return fail("args-version-future");

    a20_cluster_connect_args_v2_t v2 = {0};
    v2.base.size = sizeof(v2.base);
    v2.base.version = 2;
    v2.base.flags = A20_CONNECT_RELIABLE;
    v2.expected_boot_epoch = 1;
    if (a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&v2, 0, 0, 0, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-v2-short");
    v2.base.size = sizeof(v2);
    v2.expected_boot_epoch = 0;
    if (a20_syscall6(A20_SYS_cluster_connect, (uint64_t)&v2, 0, 0, 0, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return fail("args-v2-zero-epoch");

    /* A NULL args pointer is a fault, not a validation error. */
    r = a20_syscall6(A20_SYS_cluster_connect, 0, 0, 0, 0, 0, 0);
    if (r != -A20_ERR_FAULT)
        return fail("args-null-ptr");

    return 0;
}

/* Read the existing ownership diagnostics through the Native file ABI. */
static int task_stat(const char *key, uint64_t *out) {
    static const char path[] = "/proc/a20/task_lifetime";
    a20_path_open_args_t open = {.dir = A20_HANDLE_NULL,
                                 .rights = A20_RIGHT_READ,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = sizeof(path) - 1};
    if (a20_path_open(&open) < 0)
        return -1;
    char buf[2048];
    uint64_t got = 0;
    int status = a20_hdl_read_buf(open.out_handle, buf, sizeof(buf) - 1, &got);
    a20_hdl_close(open.out_handle);
    if (status < 0 || !got)
        return -1;
    buf[got] = 0;
    unsigned klen = a20_strlen(key);
    for (char *p = buf; *p;) {
        char *end = p;
        while (*end && *end != '\n')
            end++;
        if ((unsigned)(end - p) > klen + 2 && a20_memcmp(p, key, klen) == 0 &&
            p[klen] == ':' && p[klen + 1] == ' ') {
            uint64_t value = 0;
            for (char *d = p + klen + 2; d < end; d++) {
                if (*d < '0' || *d > '9')
                    return -1;
                value = value * 10 + (unsigned)(*d - '0');
            }
            *out = value;
            return 0;
        }
        p = *end ? end + 1 : end;
    }
    return -1;
}

/* Invoked explicitly by the full-kernel COM2/MCU byte-stream checker. */
static int uart_runtime(const char *control_path, const char *agent_path,
                        unsigned rounds) {
    a20_node_id_t self = {{0}}, peer = {{0}};
    self.bytes[15] = 1;
    peer.bytes[15] = 2;
    uint8_t hop[2] = {1, 0};
    if (a20_cluster_set_self(&self, 0) < 0 ||
        a20_cluster_route(A20_ROUTE_ADD, &peer, A20_CLX_TRANSPORT_UART, hop, 2,
                          0) < 0)
        return fail("uart-route");
    uint32_t slot = 0;
    uint64_t epoch = 0;
    uint64_t started = a20_agent_clock_ms(0);
    int discovered = 0;
    while (a20_agent_clock_ms(0) - started < 12000) {
        if (a20_cluster_resolve(&peer, "work", 4, 500, &slot, &epoch) == 0) {
            discovered = 1;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 10000000});
    }
    if (!discovered || epoch != 200 || slot != 2)
        return fail("uart-directory");
    note("UART: directory-pass\n", 21);
    /* Let the directory worker finish before taking a stable baseline. */
    a20_thread_sleep((a20_time_t){.nsecs = 100000000});
    uint64_t task_baseline = 0, zombie_baseline = 0, errors_baseline = 0;
    if (task_stat("task_objects", &task_baseline) < 0 ||
        task_stat("zombies", &zombie_baseline) < 0 ||
        task_stat("lifetime_errors", &errors_baseline) < 0)
        return fail("uart-lifetime-baseline");
    note_value("UART: tasks-before=", task_baseline);
    static uint32_t latencies[10000];
    uint64_t dot_start = a20_agent_clock_ms(0);
    for (unsigned i = 0; i < rounds; i++) {
        uint8_t data[12], request[44], reply[40];
        a20_work_put32(data, 1);
        a20_work_put32(data + 4, i);
        a20_work_put32(data + 8, (uint32_t)-7);
        a20_work_request_t q = {.job = 42,
                                .shard = i,
                                .kind = A20_WORK_OPERATOR,
                                .operator_id = A20_WORK_DOT,
                                .budget_ms = 1000,
                                .input = data,
                                .input_len = 12,
                                .attempt = 1};
        int n = a20_work_encode(request, sizeof(request), &q);
        a20_handle_t endpoint = A20_HANDLE_NULL;
        if (a20_cluster_connect_bound(&peer, 2, 200, &endpoint) < 0)
            return fail("uart-connect");
        uint32_t len = sizeof(reply);
        uint64_t call_start = a20_agent_clock_ms(0);
        int result =
            a20_cluster_exchange(endpoint, request, n, reply, &len, 3000);
        latencies[i] = (uint32_t)(a20_agent_clock_ms(0) - call_start);
        a20_hdl_close(endpoint);
        a20_work_result_t r = {0};
        int decoded = a20_work_result_decode(reply, len, &q, &r);
        if (result < 0 || decoded < 0 || r.status || r.output_len != 8 ||
            a20_work_u64(r.output) != (uint64_t)((int64_t)i * -7)) {
            note_value("UART: failed-index=", i);
            note_value("UART: failed-result=", (uint64_t)(int64_t)result);
            note_value("UART: failed-decoded=", (uint64_t)(int64_t)decoded);
            note_value("UART: failed-status=", r.status);
            return fail("uart-dot");
        }
    }
    note_value("UART: dot-elapsed-ms=", a20_agent_clock_ms(0) - dot_start);
    for (unsigned i = 1; i < rounds; i++) {
        uint32_t value = latencies[i], at = i;
        while (at && latencies[at - 1] > value) {
            latencies[at] = latencies[at - 1];
            at--;
        }
        latencies[at] = value;
    }
    note_value("UART: dot-p50-ms=", latencies[(rounds - 1) / 2]);
    note_value("UART: dot-p95-ms=", latencies[(rounds * 95 - 1) / 100]);
    note_value("UART: dot-calls=", rounds);
    note("UART: reliable-pass\n", 20);
    a20_cluster_resource_args_t resources;
    a20_memset(&resources, 0xff, sizeof(resources));
    started = a20_agent_clock_ms(0);
    do {
        if (a20_cluster_resources(&resources) < 0)
            return fail("uart-resources");
        if (!resources.values[A20_CLX_STAT_PROXIES])
            break;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    } while (a20_agent_clock_ms(0) - started < 2000);
    if (resources.values[A20_CLX_STAT_ROUTES] != 1 ||
        resources.values[A20_CLX_STAT_PROXIES] ||
        resources.values[A20_CLX_STAT_INFLIGHT] ||
        resources.values[A20_CLX_STAT_INFLIGHT_BYTES] ||
        resources.values[A20_CLX_STAT_INFLIGHT_PEAK_BYTES] < 94 ||
        !resources.values[A20_CLX_STAT_RX_PEAK_BYTES] ||
        resources.values[A20_CLX_STAT_REPLAY_BYTES] >
            resources.values[A20_CLX_STAT_MAX_REPLAY_BYTES])
        return fail("uart-resource-release");
    note_value("UART: inflight-peak-bytes=",
               resources.values[A20_CLX_STAT_INFLIGHT_PEAK_BYTES]);
    note_value("UART: rx-peak-bytes=",
               resources.values[A20_CLX_STAT_RX_PEAK_BYTES]);
    uint64_t tasks_after = 0, zombies_after = 0, errors_after = 0;
    started = a20_agent_clock_ms(0);
    do {
        a20_thread_sleep((a20_time_t){.nsecs = 10000000});
        if (task_stat("task_objects", &tasks_after) < 0 ||
            task_stat("zombies", &zombies_after) < 0 ||
            task_stat("lifetime_errors", &errors_after) < 0)
            return fail("uart-lifetime-read");
        if (tasks_after <= task_baseline && zombies_after <= zombie_baseline)
            break;
    } while (a20_agent_clock_ms(0) - started < 2000);
    note_value("UART: tasks-after=", tasks_after);
    note_value("UART: zombies-after=", zombies_after);
    note_value("UART: lifetime-errors=", errors_after);
    if (tasks_after > task_baseline || zombies_after > zombie_baseline ||
        errors_after != errors_baseline)
        return fail("uart-lifetime-release");
    note("UART: resources-pass\n", 21);
    a20_handle_t agent = A20_HANDLE_NULL, control = A20_HANDLE_NULL;
    if (spawn_service(agent_path, &agent) < 0 ||
        spawn_service(control_path, &control) < 0)
        return fail("uart-services");
    unsigned seen = 0;
    started = a20_agent_clock_ms(0);
    while (seen != 3 && a20_agent_clock_ms(0) - started < 15000) {
        a20_cluster_selection_t selected;
        if (a20_cluster_resolve_any("work", A20_WORK_OPERATOR, A20_WORK_DOT, 12,
                                    300, &selected) == 0)
            seen |= selected.node.bytes[15] == 1   ? 1
                    : selected.node.bytes[15] == 2 ? 2
                                                   : 0;
    }
    if (seen != 3)
        return fail("uart-any");
    uint8_t vectors[512], inputs[8][132];
    uint64_t expected = 0;
    for (unsigned i = 0; i < 64; i++) {
        int32_t av = (int32_t)i - 20, bv = 7 - (int32_t)i;
        a20_work_put32(vectors + 4 * i, (uint32_t)av);
        a20_work_put32(vectors + 4 * (64 + i), (uint32_t)bv);
        expected += (uint64_t)((int64_t)av * bv);
    }
    a20_job_task_t tasks[A20_JOB_DOT_TASKS];
    int count =
        a20_job_dot_graph(tasks, A20_JOB_DOT_TASKS, inputs, vectors, 64);
    a20_job_t job;
    if (count != 7 ||
        a20_job_init(&job, 1000, tasks, count, a20_agent_clock_ms(0), 20000,
                     1500, 4) < 0 ||
        a20_cluster_job_run(&job, "work") < 0 || job.state != A20_JOB_DONE ||
        tasks[count - 1].result != expected)
        return fail("uart-tree");
    seen = 0;
    for (int i = 0; i < count; i++)
        seen |= tasks[i].target.node_id[15] == 1   ? 1
                : tasks[i].target.node_id[15] == 2 ? 2
                                                   : 0;
    stop_service(control);
    stop_service(agent);
    if (seen != 3)
        return fail("uart-tree-mixed");
    note("UART: mixed-tree-pass\nUART: PASS\n", 33);
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    if (si)
        g_stdout = si->stdout_handle;

    if ((argc == 4 || argc == 5) && !a20_strcmp(argv[1], "uart")) {
        unsigned rounds = 96;
        if (argc == 5) {
            rounds = 0;
            for (const char *p = argv[4]; *p; p++) {
                if (*p < '0' || *p > '9' || rounds > 10000)
                    return fail("uart-rounds");
                rounds = rounds * 10 + (unsigned)(*p - '0');
            }
            if (rounds < 96 || rounds > 10000)
                return fail("uart-rounds");
        }
        return uart_runtime(argv[2], argv[3], rounds);
    }

    note("start\n", 6);

    if (abi_numbers() != 0)
        return 1;
    note("abi-nr ok\n", 10);

    if (abi_node_hash() != 0)
        return 1;
    note("abi-node ok\n", 11);

    if (abi_live_identity() != 0)
        return 1;
    note("abi-live ok\n", 11);

    if (abi_snapshot_and_agent() != 0)
        return 1;
    note("abi-agent ok\n", 13);

    if (abi_clusterd() != 0)
        return 1;

    if (abi_arg_rejection() != 0)
        return 1;
    note("abi-args ok\n", 11);

    if (abi_struct_versioning() != 0)
        return 1;
    note("abi-ver ok\n", 10);

    note("NATIVE_CLUSTER: PASS\n", 20);
    return 0;
}
