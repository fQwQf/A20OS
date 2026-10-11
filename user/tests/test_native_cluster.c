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
#include "../../kernel/include/uapi/a20/cluster_job.h"

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

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    if (si)
        g_stdout = si->stdout_handle;

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
