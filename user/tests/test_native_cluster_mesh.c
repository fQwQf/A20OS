/* Three independent Native instances; the host fixture removes/restarts node
 * B and selectively blocks A->B directory requests to exercise indirect C. */
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
    const char *argv[7] = {"/bin/clusterd-rv", identities[role], 0, 0, 0, 0, 0};
    unsigned next = 2;
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
static int member(unsigned wanted, uint32_t *state, uint64_t *epoch) {
    a20_node_id_t id;
    a20_node_id_parse_hex(identities[wanted], &id);
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect(0, 0, "cluster.control", 0, 0, &ep) < 0)
        return -1;
    int found = -1;
    for (unsigned i = 0; i < 32; i++) {
        uint8_t query[96], reply[80];
        uint32_t len = 80;
        a20_control_request(query, A20_CONTROL_MEMBER, i, 0, 0, 0, 0, i + 1);
        if (a20_cluster_exchange(ep, query, 96, reply, &len, 200) < 0)
            break;
        if (len != 80 || reply[0] != 'C' || reply[1] != 'M' || reply[2] != 1 ||
            reply[3] != A20_CONTROL_MEMBER || a20_work_u64(reply + 64) != i + 1)
            break;
        if (!a20_work_u32(reply + 4) && !a20_memcmp(reply + 8, id.bytes, 16)) {
            *state = a20_work_u32(reply + 36);
            *epoch = a20_work_u64(reply + 24);
            found = 0;
            break;
        }
    }
    a20_hdl_close(ep);
    return found;
}
static int wait_member(unsigned node, uint32_t state, uint64_t different_epoch,
                       unsigned budget, uint64_t *out) {
    uint64_t start = a20_agent_clock_ms(0);
    while (a20_agent_clock_ms(0) - start < budget) {
        uint32_t actual;
        uint64_t epoch;
        if (member(node, &actual, &epoch) == 0 && actual == state &&
            (!different_epoch || epoch != different_epoch)) {
            if (out)
                *out = epoch;
            return 0;
        }
        sleep_ms(20);
    }
    return -1;
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
    return a20_cluster_exchange(ep, wire, n, reply, &len, 1000) >= 0 &&
                   a20_work_result_decode(reply, len, &request, &result) == 0 &&
                   !result.status && result.output_len == 8 &&
                   (int64_t)a20_work_u64(result.output) == -42
               ? 0
               : -1;
}
static int connect_node(unsigned wanted, a20_handle_t *ep) {
    a20_node_id_t node;
    uint32_t slot;
    uint64_t epoch;
    a20_node_id_parse_hex(identities[wanted], &node);
    if (a20_cluster_resolve(&node, "work", 4, 1000, &slot, &epoch) < 0)
        return -1;
    if (wanted == role)
        a20_node_id_local(&node);
    return a20_cluster_connect_bound(&node, slot, epoch, ep) >= 0 ? 0 : -1;
}
static int any_set(unsigned expected) {
    unsigned seen = 0;
    for (unsigned i = 0; i < 18; i++) {
        a20_handle_t ep = A20_HANDLE_NULL;
        a20_cluster_selection_t s;
        if (a20_cluster_connect_uri("service://any/work", A20_WORK_OPERATOR, 1,
                                    12, 300, &ep, &s) < 0)
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
static int tree(unsigned expected_nodes, int expect_retry) {
    uint8_t vectors[1024], inputs[8][132];
    a20_job_task_t tasks[15];
    a20_job_t job;
    uint64_t expected = 0;
    for (unsigned i = 0; i < 128; i++) {
        a20_work_put32(vectors + i * 4, (uint32_t)(int32_t)((int)i - 64));
        a20_work_put32(vectors + (128 + i) * 4, i);
        expected += (uint64_t)((int64_t)((int)i - 64) * i);
    }
    int n = a20_job_dot_graph(tasks, 15, inputs, vectors, 128);
    if (n != 15 ||
        a20_job_init(&job, 1000, tasks, n, a20_agent_clock_ms(0), 6000, 1000,
                     8) < 0 ||
        a20_cluster_job_run(&job, "work") < 0)
        return -1;
    if (expect_retry && job.retries)
        note("MESH: job-retry-pass\n");
    unsigned seen = 0;
    for (unsigned i = 0; i < 15; i++) {
        unsigned node = tasks[i].target.node_id[15] == 0x1f   ? 0
                        : tasks[i].target.node_id[15] == 0x2f ? 1
                                                              : 2;
        seen |= 1u << node;
    }
    return tasks[14].result == expected && seen == expected_nodes &&
                   (!expect_retry || job.retries)
               ? 0
               : -1;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    if (argc != 2 || argv[1][0] < '0' || argv[1][0] > '2' || argv[1][1])
        return 1;
    role = argv[1][0] - '0';
    a20_handle_t agent = A20_HANDLE_NULL, control = A20_HANDLE_NULL;
    const char *agent_argv[] = {"/bin/cluster-agent-rv", identities[role], 0};
    if (spawn(agent_argv[0], agent_argv, 2, &agent) < 0)
        goto fail;
    sleep_ms(100);
    if (start_control(&control) < 0)
        goto fail;
    if (role) {
        note("MESH: server-ready\n");
        for (;;)
            sleep_ms(1000);
    }
    uint64_t old_epoch;
    if (wait_member(1, A20_MEMBER_ALIVE, 0, 10000, &old_epoch) < 0 ||
        wait_member(2, A20_MEMBER_ALIVE, 0, 10000, 0) < 0)
        goto fail;
    sleep_ms(1500);
    if (any_set(7) < 0 || tree(7, 1) < 0)
        goto fail;
    a20_handle_t old_b = A20_HANDLE_NULL;
    if (connect_node(1, &old_b) < 0 || dot(old_b) < 0)
        goto fail;
    note("MESH: indirect-ready\n");
    sleep_ms(3000);
    if (wait_member(1, A20_MEMBER_ALIVE, 0, 1000, 0) < 0)
        goto fail;
    note("MESH: indirect-pass\n");
    if (wait_member(1, A20_MEMBER_DEAD, 0, 12000, 0) < 0 || any_set(5) < 0 ||
        tree(5, 0) < 0 || dot(old_b) == 0)
        goto fail;
    note("MESH: reboot-ready\n");
    if (wait_member(1, A20_MEMBER_ALIVE, old_epoch, 20000, 0) < 0)
        goto fail;
    sleep_ms(1500);
    if (any_set(7) < 0 || dot(old_b) == 0)
        goto fail;
    a20_hdl_close(old_b);
    a20_handle_t c = A20_HANDLE_NULL;
    if (connect_node(2, &c) < 0)
        goto fail;
    stop(control);
    control = A20_HANDLE_NULL;
    if (dot(c) < 0 || start_control(&control) < 0 ||
        wait_member(2, A20_MEMBER_ALIVE, 0, 10000, 0) < 0 || dot(c) < 0)
        goto fail;
    a20_hdl_close(c);
    note("NATIVE_CLUSTER_MESH: PASS\n");
    stop(control);
    stop(agent);
    return 0;
fail:
    note("NATIVE_CLUSTER_MESH: FAIL\n");
    stop(control);
    stop(agent);
    return 1;
}
