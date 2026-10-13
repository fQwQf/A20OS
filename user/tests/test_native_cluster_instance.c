/* Mixed-ISA actual-kernel namespace admission and ordinary service RPC. */
#include "liba20rt/a20_cluster_job.h"
#include "liba20rt/crt0_a20.h"
static a20_handle_t output = A20_HANDLE_NULL;
static const char *identities[] = {"101112131415161718191a1b1c1d1e1f",
                                   "202122232425262728292a2b2c2d2e2f",
                                   "303132333435363738393a3b3c3d3e3f"};
static const char *addresses[] = {"10.0.3.2:44020", "10.0.3.3:44020",
                                  "10.0.3.4:44020"};
static unsigned role;
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static void sleep_ms(unsigned n) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)n * 1000000});
}
static int spawn(const char *path, const char **argv, unsigned argc,
                 a20_handle_t *task) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path),
                                 .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    if (a20_path_open(&file) < 0)
        return -1;
    a20_task_spawn_args_t args = {0};
    args.size = sizeof(args);
    args.version = 2;
    args.image = file.out_handle;
    args.root_dir = args.cwd_dir = args.event_queue = A20_HANDLE_NULL;
    args.stdin_handle = A20_HANDLE_NULL;
    args.stdout_handle = args.stderr_handle = output;
    args.argv = (uint64_t)(uintptr_t)argv;
    args.argc = argc;
    int ok =
        a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0) >= 0;
    a20_hdl_close(file.out_handle);
    if (ok)
        *task = args.out_task;
    return ok ? 0 : -1;
}
static int start_control(a20_handle_t *task) {
    const char *argv[9] = {"/bin/clusterd-" A20_CLUSTER_ARCH_TAG,
                           identities[role], "--timing-scale", "10"};
    unsigned next = 4;
    for (unsigned i = 0; i < 3; i++)
        if (i != role) {
            argv[next++] = identities[i];
            argv[next++] = addresses[i];
        }
    return spawn(argv[0], argv, next, task);
}
static void stop(a20_handle_t task) {
    if (task == A20_HANDLE_NULL)
        return;
    a20_task_kill(task, 9);
    a20_task_status_t status;
    for (unsigned i = 0; i < 1000; i++) {
        if (a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status) >= 0)
            break;
        sleep_ms(1);
    }
    a20_hdl_close(task);
}
static int dot(a20_handle_t ep) {
    uint8_t input[12], wire[44], reply[40];
    a20_work_put32(input, 1);
    a20_work_put32(input + 4, (uint32_t)-7);
    a20_work_put32(input + 8, 6);
    a20_work_request_t request = {.kind = A20_WORK_OPERATOR,
                                  .job = 900,
                                  .operator_id = 1,
                                  .budget_ms = 500,
                                  .input_len = 12,
                                  .input = input};
    int n = a20_work_encode(wire, sizeof(wire), &request);
    uint32_t len = sizeof(reply);
    a20_work_result_t result;
    return a20_cluster_exchange(ep, wire, n, reply, &len, 4000) >= 0 &&
                   a20_work_result_decode(reply, len, &request, &result) == 0 &&
                   !result.status && result.output_len == 8 &&
                   (int64_t)a20_work_u64(result.output) == -42
               ? 0
               : -1;
}
static int any_set(unsigned expected) {
    unsigned seen = 0;
    for (unsigned i = 0; i < 18; i++) {
        a20_handle_t ep = A20_HANDLE_NULL;
        a20_cluster_selection_t s;
        if (a20_cluster_connect_uri("service://any/work", A20_WORK_OPERATOR, 1,
                                    12, 4000, &ep, &s) < 0)
            return -1;
        int ok = dot(ep);
        a20_hdl_close(ep);
        if (ok < 0)
            return -1;
        unsigned node = s.node.bytes[15] == 0x1f   ? 0
                        : s.node.bytes[15] == 0x2f ? 1
                                                   : 2;
        seen |= 1u << node;
        if (!(expected & (1u << node)))
            return -1;
    }
    return seen == expected ? 0 : -1;
}

static int bind_instance(unsigned epoch, int foreign) {
    a20_runtime_binding_t b = {.size = 64, .version = 1, .boot_epoch = epoch};
    a20_node_id_t domain;
    a20_node_id_parse_hex(identities[role], &domain);
    a20_memcpy(b.domain_id, domain.bytes, 16);
    b.instance_id[0] = foreign ? 0x99 : 0x88;
    return a20_runtime_bind(&b) < 0 ? -1 : 0;
}
static int runtime_of(unsigned wanted, unsigned state, unsigned epoch) {
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[wanted], &node);
    uint64_t started = a20_agent_clock_ms(0);
    do {
        a20_runtime_info_t info;
        uint64_t transport = 0;
        if (a20_cluster_runtime_query(&node, 1000, &info, &transport) >= 0 &&
            transport && info.state == state && info.boot_epoch == epoch)
            return 0;
        sleep_ms(50);
    } while (a20_agent_clock_ms(0) - started < 12000);
    return -1;
}
static int proof_once(unsigned wanted, unsigned epoch) {
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[wanted], &node);
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect(0, 0, "cluster.control", 0, 0, &ep) < 0)
        return -1;
    uint8_t query[96], reply[144];
    uint32_t len = 144;
    a20_control_runtime_request(query, A20_CONTROL_INDIRECT, 0, 0, 0,
                                (const char *)node.bytes, 16, 123);
    a20_runtime_info_t remote, local;
    int ok = a20_cluster_exchange(ep, query, 96, reply, &len, 4000) >= 0 &&
             !a20_control_runtime_decode(reply, len, &remote) &&
             a20_work_u64(reply + 64) == 123 && remote.boot_epoch == epoch &&
             a20_runtime_info(&local) >= 0 &&
             a20_runtime_admits(&local, &remote, node.bytes);
    a20_hdl_close(ep);
    return ok ? 0 : -1;
}
static int proof(unsigned wanted, unsigned epoch) {
    if (wanted == 2)
        return proof_once(wanted, epoch);
    uint64_t started = a20_agent_clock_ms(0);
    do {
        if (!proof_once(wanted, epoch))
            return 0;
        sleep_ms(50);
    } while (a20_agent_clock_ms(0) - started < 5000);
    return -1;
}
static int remote_proof(void) {
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[1], &node);
    uint32_t slot;
    uint64_t epoch;
    if (a20_cluster_resolve(&node, "cluster.control", 15, 4000, &slot, &epoch) <
        0)
        return -1;
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect_bound(&node, slot, epoch, &ep) < 0)
        return -1;
    uint8_t query[96], reply[144];
    uint32_t len = 144;
    a20_control_runtime_request(query, A20_CONTROL_INDIRECT, 0, 0, 0,
                                (const char *)node.bytes, 16, 456);
    a20_runtime_info_t remote;
    int ok = a20_cluster_exchange(ep, query, 96, reply, &len, 4000) >= 0 &&
             !a20_control_runtime_decode(reply, len, &remote) &&
             remote.boot_epoch == 200 && a20_work_u64(reply + 64) == 456;
    a20_hdl_close(ep);
    return ok ? 0 : -1;
}
static int wait_set(unsigned expected, unsigned budget) {
    uint64_t start = a20_agent_clock_ms(0);
    do {
        if (!any_set(expected))
            return 0;
        sleep_ms(100);
    } while (a20_agent_clock_ms(0) - start < budget);
    return -1;
}
static void number(uint64_t value) {
    char text[18] = "0000000000000000\n";
    for (unsigned i = 0; i < 16; i++)
        text[15 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    note(text);
}
static void diagnose(void) {
    for (unsigned i = 0; i < 3; i++) {
        a20_node_id_t node;
        a20_node_id_parse_hex(identities[i], &node);
        a20_runtime_info_t info;
        uint64_t transport;
        int status = a20_cluster_runtime_query(&node, 1000, &info, &transport);
        note("DIAG CD node/status ");
        number(i);
        number((int64_t)status);
        if (status >= 0) {
            number(info.state);
            number(info.boot_epoch);
        }
    }
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect(0, 0, "cluster.control", 0, 0, &ep) >= 0) {
        for (unsigned i = 0; i < 5; i++) {
            uint8_t query[96], reply[80];
            uint32_t len = 80;
            a20_control_request(query, A20_CONTROL_MEMBER, i, 0, 0, 0, 0,
                                i + 1);
            int status = a20_cluster_exchange(ep, query, 96, reply, &len, 4000);
            note("DIAG CM index/status ");
            number(i);
            number((int64_t)status);
            if (status >= 0) {
                number(a20_work_u32(reply + 4));
                number(reply[23]);
                number(a20_work_u32(reply + 36));
            }
        }
        a20_hdl_close(ep);
    }
}
static int failed(const char *step) {
    diagnose();
    note("INSTANCE: FAIL ");
    note(step);
    note("\n");
    return 1;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    if (argc != 3 || argv[1][0] < '0' || argv[1][0] > '2')
        return 1;
    role = argv[1][0] - '0';
    unsigned mode = argv[2][0] - '0';
    /* A activates after legacy discovery; B's first reboot deliberately reuses
     * epoch 200, its second uses 201. C first foreign, then explicitly UNBOUND. */
    if (role == 1 && bind_instance(mode == 2 ? 201 : 200, 0))
        return failed("bind-B");
    if (role == 2 && mode == 0 && bind_instance(300, 1))
        return failed("bind-C");
    a20_handle_t agent = A20_HANDLE_NULL, control = A20_HANDLE_NULL;
    const char *agent_argv[] = {"/bin/cluster-agent-" A20_CLUSTER_ARCH_TAG,
                                identities[role], 0};
    if (spawn(agent_argv[0], agent_argv, 2, &agent))
        return failed("spawn-agent");
    sleep_ms(100);
    if (start_control(&control))
        return failed("spawn-control");
    if (role) {
        note("INSTANCE: peer-ready\n");
        for (;;) {
            a20_task_status_t status = {.size = sizeof(status), .version = 1};
            if (a20_task_wait(agent, A20_TASK_WAIT_NONBLOCK, &status) >= 0) {
                note("DIAG agent exit ");
                number(status.exit_code);
                return failed("peer-agent-exited");
            }
            if (a20_task_wait(control, A20_TASK_WAIT_NONBLOCK, &status) >= 0) {
                note("DIAG control exit ");
                number(status.exit_code);
                return failed("peer-control-exited");
            }
            sleep_ms(1000);
        }
    }
    if (wait_set(7, 15000))
        return failed("legacy-selection");
    note("INSTANCE: legacy-pass\n");
    if (bind_instance(100, 0))
        return failed("bind-A");
    if (wait_set(3, 10000))
        return failed("same-instance-selection");
    if (runtime_of(1, 1, 200))
        return failed("CD2-B");
    if (runtime_of(2, 1, 300))
        return failed("CD2-C");
    if (proof(1, 200))
        return failed("CM2-B");
    if (remote_proof())
        return failed("remote-CM2");
    if (!proof(2, 300))
        return failed("CM2-foreign");
    note("INSTANCE: bound-pass\n");
    /* Host removes B and reboots C unbound. */
    sleep_ms(4000);
    if (runtime_of(2, 0, 0) || wait_set(1, 12000))
        return failed("unbound-filter");
    note("INSTANCE: reused-ready\n");
    sleep_ms(10000);
    if (runtime_of(1, 1, 200) || wait_set(1, 8000))
        return failed("reused-epoch-filter");
    note("INSTANCE: reused-pass\n");
    sleep_ms(10000);
    if (runtime_of(1, 1, 201))
        return failed("fresh-CD2");
    if (wait_set(3, 15000))
        return failed("fresh-selection");
    if (proof(1, 201))
        return failed("fresh-CM2");
    note("INSTANCE: fresh-pass\n");
    stop(control);
    control = A20_HANDLE_NULL;
    if (start_control(&control) || wait_set(3, 15000) || proof(1, 201))
        return failed("owner-restart");
    note("INSTANCE: PASS\n");
    stop(control);
    stop(agent);
    return 0;
}
