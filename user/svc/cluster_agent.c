/* Native work agent. Optional pre-opened executable id 1 supports process
 * tasks; no remote payload is interpreted as a filesystem path. */
#include "liba20rt/a20_cluster_agent.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t process_image = A20_HANDLE_NULL;
static uint32_t run_process(void *ctx, const a20_work_request_t *request,
                            uint8_t *output, uint32_t *output_len) {
    (void)ctx;
    if (request->operator_id != 1)
        return A20_WORK_UNSUPPORTED;
    if (request->input_len || *output_len < 8)
        return A20_WORK_INVALID;
    uint64_t started = a20_agent_clock_ms(0);
    a20_task_spawn_args_t spawn = {0};
    spawn.image = process_image;
    spawn.flags = A20_SPAWN_PARENT_BOUND;
    spawn.root_dir = spawn.cwd_dir = spawn.event_queue = A20_HANDLE_NULL;
    if (a20_task_spawn(&spawn) < 0)
        return A20_WORK_FAILED;
    uint32_t result = A20_WORK_OK;
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    for (;;) {
        a20_status_t r =
            a20_task_wait(spawn.out_task, A20_TASK_WAIT_NONBLOCK, &status);
        if (!r)
            break;
        if (r != -A20_ERR_WOULD_BLOCK) {
            result = A20_WORK_FAILED;
            break;
        }
        if (a20_agent_clock_ms(0) - started >= request->budget_ms) {
            result = A20_WORK_DEADLINE;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    if (result) {
        if (a20_task_kill(spawn.out_task, 9) < 0)
            result = A20_WORK_FAILED;
        /* Reap a canceled child, with a separate bounded cleanup budget. */
        uint64_t cleanup_started = a20_agent_clock_ms(0);
        while (a20_task_wait(spawn.out_task, A20_TASK_WAIT_NONBLOCK, &status) ==
               -A20_ERR_WOULD_BLOCK) {
            if (a20_agent_clock_ms(0) - cleanup_started >= 1000) {
                result = A20_WORK_FAILED;
                break;
            }
            a20_thread_sleep((a20_time_t){.nsecs = 1000000});
        }
    }
    a20_hdl_close(spawn.out_task);
    if (!result) {
        a20_work_put32(output, (uint32_t)status.exit_code);
        a20_work_put32(output + 4, status.exit_reason);
        *output_len = 8;
    }
    return result;
}

static int parse_identity(const char *text, a20_node_id_t *id) {
    if (!text || a20_strlen(text) != 32)
        return -1;
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = text[i * 2 + j];
            unsigned digit;
            if (c >= '0' && c <= '9')
                digit = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = (unsigned)(c - 'A' + 10);
            else
                return -1;
            value = value * 16 + digit;
        }
        id->bytes[i] = (uint8_t)value;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_cluster_snapshot_args_t snapshot = {0};
    a20_status_t r = a20_cluster_snapshot(&snapshot);
    if (r < 0) {
        a20_node_id_t id;
        if (argc < 2 || parse_identity(argv[1], &id) < 0 ||
            a20_cluster_set_self(&id, 0) < 0)
            return 1;
    } else if (argc >= 2) {
        a20_node_id_t id;
        if (parse_identity(argv[1], &id) < 0 ||
            a20_memcmp(id.bytes, snapshot.node_id, 16))
            return 2;
    }
    if (argc >= 3) {
        a20_path_open_args_t image = {0};
        image.dir = A20_HANDLE_NULL;
        image.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
        image.path = (uint64_t)(uintptr_t)argv[2];
        image.path_len = (uint32_t)a20_strlen(argv[2]);
        if (a20_path_open(&image) < 0)
            return 3;
        process_image = image.out_handle;
    }
    const a20_work_operator_t ops[] = {
        {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 1024, 8, a20_work_dot, 0},
        {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 1024, 8,
         a20_work_sum64, 0},
    };
    a20_cluster_agent_server_t server = {0};
    server.agent = (a20_work_agent_t){
        .operators = ops,
        .operator_count = 2,
        .max_input = 1024,
        .max_output = 1024,
        .max_inflight = 1,
        .max_budget_ms = 30000,
        .process = process_image != A20_HANDLE_NULL ? run_process : 0,
        .clock_ms = a20_agent_clock_ms};
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], "work",
                           A20_EXPORT_REPLACE | A20_EXPORT_WORK, 0) < 0)
        return 4;
    a20_hdl_close(pair.endpoints[1]);
    a20_work_agent_t agent = server.agent;
    a20_cluster_agent_init(&server, pair.endpoints[0], &agent);
    a20_start_info_t *start = a20_get_start_info();
    if (start && start->stdout_handle != A20_HANDLE_NULL)
        a20_hdl_write_buf(start->stdout_handle, "CLUSTER_AGENT: ready\n", 21,
                          0);
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            return 0;
        r = a20_cluster_agent_pump(&server);
        if (r < 0)
            return 5;
        if (!r)
            a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
}
