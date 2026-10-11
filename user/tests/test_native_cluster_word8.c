/* Eight-node wordcount acceptance. The host fixture owns node failure and the
 * two optional MCU byte streams; all scheduling uses the production SDK. */
#include "liba20rt/a20_cluster_job.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_word_job.h"
static a20_handle_t output = A20_HANDLE_NULL;
static a20_job_task_t tasks[A20_JOB_WORD_TASKS];
static uint8_t inputs[A20_JOB_WORD_LEAVES][A20_JOB_WORD_CHUNK];
static uint8_t outputs[A20_JOB_WORD_TASKS][A20_WORD_RESULT];
static uint8_t text[A20_JOB_MAX_TEXT], reference[A20_WORD_RESULT];
static uint32_t text_len, reference_len;
static a20_cluster_selection_t members[8];
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static void sleep_ms(unsigned ms) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)ms * 1000000});
}
static int spawn(const char *path, const char **argv, unsigned argc,
                 a20_handle_t *task) {
    a20_path_open_args_t image = {.dir = A20_HANDLE_NULL,
                                  .path = (uint64_t)(uintptr_t)path,
                                  .path_len = (uint32_t)a20_strlen(path),
                                  .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    if (a20_path_open(&image) < 0)
        return -1;
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .flags = A20_SPAWN_PARENT_BOUND,
                                  .image = image.out_handle,
                                  .root_dir = A20_HANDLE_NULL,
                                  .cwd_dir = A20_HANDLE_NULL,
                                  .event_queue = A20_HANDLE_NULL,
                                  .stdin_handle = A20_HANDLE_NULL,
                                  .stdout_handle = output,
                                  .stderr_handle = output,
                                  .argv = (uint64_t)(uintptr_t)argv,
                                  .argc = argc};
    int r = a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0);
    a20_hdl_close(image.out_handle);
    if (r >= 0)
        *task = args.out_task;
    return r < 0 ? -1 : 0;
}
static int reap(a20_handle_t task, unsigned budget) {
    uint64_t start = a20_agent_clock_ms(0);
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    do {
        int r = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (r >= 0) {
            a20_hdl_close(task);
            return status.exit_code ? -1 : 0;
        }
        if (r != -A20_ERR_WOULD_BLOCK)
            break;
        sleep_ms(1);
    } while (a20_agent_clock_ms(0) - start < budget);
    a20_task_kill(task, 9);
    a20_task_wait(task, 0, &status);
    a20_hdl_close(task);
    return -1;
}
static int topology(unsigned role, int mixed) {
    a20_node_id_t self = {{0}};
    self.bytes[15] = role + 1;
    if (a20_cluster_set_self(&self, A20_CLUSTER_CAP_RELIABLE |
                                        A20_CLUSTER_CAP_RELAY) < 0)
        return -1;
    for (unsigned id = 1; id <= 8; id++) {
        if (id == role + 1)
            continue;
        a20_node_id_t peer = {{0}};
        peer.bytes[15] = id;
        if (mixed && id >= 7 && role == id - 3) {
            uint8_t uart[2] = {1, 0};
            if (a20_cluster_route(A20_ROUTE_ADD, &peer, A20_CLX_TRANSPORT_UART,
                                  uart, 2, 0) < 0)
                return -1;
        } else {
            unsigned next = mixed && id >= 7 ? id - 2 : id;
            uint8_t udp[6] = {10, 0, 3, next + 1, 0xab, 0xf4};
            if (a20_cluster_route(A20_ROUTE_ADD, &peer, A20_CLX_TRANSPORT_UDP,
                                  udp, 6, next == id ? 0 : 1) < 0)
                return -1;
        }
    }
    return 0;
}
static int all_members(void) {
    unsigned seen = 0;
    uint64_t start = a20_agent_clock_ms(0);
    while (seen != 255 && a20_agent_clock_ms(0) - start < 25000) {
        a20_cluster_selection_t s;
        if (!a20_cluster_resolve_any("work", A20_WORK_OPERATOR,
                                     A20_WORK_WORD_MAP, A20_JOB_WORD_CHUNK, 200,
                                     &s)) {
            unsigned id = s.node.bytes[15];
            if (id >= 1 && id <= 8) {
                seen |= 1u << (id - 1);
                members[id - 1] = s;
            }
        }
        sleep_ms(1);
    }
    return seen == 255 ? 0 : -1;
}
static int read_text(void) {
    const char *path = "/bin/wordcount.txt";
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path),
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return -1;
    uint64_t got = 0;
    int r = a20_hdl_read_buf(file.out_handle, text, sizeof(text), &got);
    a20_hdl_close(file.out_handle);
    if (r < 0 || got != 1014)
        return -1;
    text_len = got;
    a20_work_request_t map = {.input = text, .input_len = text_len};
    reference_len = sizeof(reference);
    return a20_work_word_map(0, &map, reference, &reference_len) ? -1 : 0;
}
static int tree(uint64_t id, int fault) {
    int n = a20_job_word_graph(tasks, A20_JOB_WORD_TASKS, inputs, outputs, text,
                               text_len);
    a20_job_t job;
    if (n != 31 || a20_job_init(&job, id, tasks, n, a20_agent_clock_ms(0),
                                30000, 1500, 8) < 0)
        return -1;
    /* Any selection is round robin over current eligible services, not an
     * execution-coverage promise. Pin one baseline MAP to each discovered
     * member using the same admission/completion API, then run the remaining
     * graph with the production Native driver. The fault job uses any only. */
    for (unsigned i = 0; !fault && i < 8; i++) {
        a20_cluster_selection_t *s = &members[i];
        a20_job_target_t target = {.boot_epoch = s->boot_epoch,
                                   .slot = s->slot,
                                   .max_input = s->max_input,
                                   .max_output = s->max_output,
                                   .max_inflight = s->max_inflight,
                                   .flags = s->flags};
        a20_memcpy(target.node_id, s->node.bytes, 16);
        uint8_t wire[32 + A20_JOB_MAX_INPUT], reply[200];
        int len = a20_job_begin(&job, i, &target, a20_agent_clock_ms(0), wire,
                                sizeof(wire));
        a20_node_id_t node = s->node;
        if (!i)
            a20_node_id_local(&node);
        a20_handle_t ep = A20_HANDLE_NULL;
        if (len < 0 ||
            a20_cluster_connect_bound(&node, s->slot, s->boot_epoch, &ep) < 0)
            return -1;
        uint32_t got = sizeof(reply);
        int r = a20_cluster_exchange(ep, wire, len, reply, &got, 1400);
        a20_hdl_close(ep);
        if (r < 0 || a20_job_complete(&job, i, target.node_id, s->boot_epoch,
                                      reply, got, a20_agent_clock_ms(0)) < 0) {
            note("WORD8: pinned-baseline-failed\n");
            return -1;
        }
    }
    if (a20_cluster_job_run(&job, "work") < 0 || job.completed != 31 ||
        tasks[30].output_len != reference_len ||
        a20_memcmp(tasks[30].output, reference, reference_len) ||
        (fault && !job.retries)) {
        note("WORD8: tree-state-or-result-failed\n");
        return -1;
    }
    unsigned seen = 0;
    for (unsigned i = 0; i < 31; i++) {
        unsigned node = tasks[i].target.node_id[15];
        if (node >= 1 && node <= 8)
            seen |= 1u << (node - 1);
    }
    char assignments[31 * 4 + 2];
    for (unsigned i = 0; i < 31; i++) {
        unsigned node = tasks[i].target.node_id[15], attempt = tasks[i].attempt;
        assignments[4 * i] = "0123456789abcdef"[node >> 4];
        assignments[4 * i + 1] = "0123456789abcdef"[node & 15];
        assignments[4 * i + 2] = "0123456789abcdef"[attempt >> 4];
        assignments[4 * i + 3] = "0123456789abcdef"[attempt & 15];
    }
    assignments[31 * 4] = '\n';
    assignments[31 * 4 + 1] = 0;
    note("WORD8: assignments=");
    note(assignments);
    if (!fault && seen != 255) {
        note("WORD8: missing-baseline-node\n");
        return -1;
    }
    char encoded[2 * A20_WORD_RESULT + 2];
    for (unsigned i = 0; i < reference_len; i++) {
        encoded[2 * i] = "0123456789abcdef"[tasks[30].output[i] >> 4];
        encoded[2 * i + 1] = "0123456789abcdef"[tasks[30].output[i] & 15];
    }
    encoded[2 * reference_len] = '\n';
    encoded[2 * reference_len + 1] = 0;
    note("WORD8: result=");
    note(encoded);
    return 0;
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    if (argc != 6 || argv[1][0] < '0' || argv[1][0] > '7' || argv[1][1] ||
        (a20_strcmp(argv[2], "servers") && a20_strcmp(argv[2], "mixed")))
        return 1;
    unsigned role = argv[1][0] - '0';
    int mixed = !a20_strcmp(argv[2], "mixed");
    a20_handle_t agent = A20_HANDLE_NULL, control = A20_HANDLE_NULL;
    const char *agent_argv[] = {argv[3], 0}, *control_argv[] = {argv[4], 0};
    if ((mixed && role >= 6) || topology(role, mixed) < 0 ||
        spawn(argv[3], agent_argv, 1, &agent) < 0 ||
        spawn(argv[4], control_argv, 1, &control) < 0)
        goto fail;
    if (role) {
        note("WORD8: server-ready\n");
        for (;;)
            sleep_ms(1000);
    }
    if (read_text() < 0 || all_members() < 0 || tree(10000, 0) < 0)
        goto fail;
    note("WORD8: baseline-pass\n");
    note("WORD8: failure-ready\n");
    if (tree(10001, 1) < 0)
        goto fail;
    note("WORD8: failure-pass\n");
    const char *cli_argv[] = {argv[5], "/bin/wordcount.txt", 0};
    a20_handle_t submitter = A20_HANDLE_NULL, jobs = A20_HANDLE_NULL;
    /* Derive jobd from the architecture-specific control path. */
    char jobd[64] = "/bin/jobd-";
    if (a20_strlen(argv[4]) < 15 || a20_memcmp(argv[4], "/bin/clusterd-", 14))
        goto fail;
    const char *suffix = argv[4] + 14;
    if (a20_strlen(suffix) + 11 >= sizeof(jobd))
        goto fail;
    a20_memcpy(jobd + 10, suffix, a20_strlen(suffix) + 1);
    const char *job_argv[] = {jobd, 0};
    if (spawn(jobd, job_argv, 1, &jobs) < 0)
        goto fail;
    sleep_ms(100);
    if (spawn(argv[5], cli_argv, 2, &submitter) < 0 ||
        reap(submitter, 12000) < 0)
        goto fail;
    a20_task_kill(jobs, 15);
    reap(jobs, 3000);
    a20_task_kill(control, 15);
    reap(control, 3000);
    a20_task_kill(agent, 15);
    reap(agent, 3000);
    note("NATIVE_CLUSTER_WORD8: PASS\n");
    return 0;
fail:
    note("NATIVE_CLUSTER_WORD8: FAIL\n");
    if (control != A20_HANDLE_NULL) {
        a20_task_kill(control, 9);
        reap(control, 2000);
    }
    if (agent != A20_HANDLE_NULL) {
        a20_task_kill(agent, 9);
        reap(agent, 2000);
    }
    return 1;
}
