/* Real persisted-config, crash/restart, singleton and graceful cleanup test. */
#include "liba20rt/a20_cluster_client.h"
#include "liba20rt/crt0_a20.h"
static a20_handle_t output = A20_HANDLE_NULL;
static const char path[] = "/tmp/a20-cluster-supervisor.conf";
static const char supervisor[] = "/bin/cluster-supervisor-rv";
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static int fail(const char *s) {
    note("SUPERVISOR: FAIL ");
    note(s);
    note("\n");
    return 1;
}
static void sleep_ms(unsigned n) {
    a20_thread_sleep((a20_time_t){.nsecs = (uint64_t)n * 1000000});
}
static int write_config(const char *bytes) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = sizeof(path) - 1,
                                 .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
                                 .flags = A20_PATH_OPEN_CREATE |
                                          A20_PATH_OPEN_RDWR |
                                          A20_PATH_OPEN_TRUNC,
                                 .mode = 0600};
    if (a20_path_open(&file) < 0)
        return -1;
    uint64_t wrote = 0;
    unsigned len = a20_strlen(bytes);
    int r = a20_hdl_write_buf(file.out_handle, bytes, len, &wrote);
    a20_hdl_close(file.out_handle);
    return r < 0 || wrote != len ? -1 : 0;
}
static int spawn(a20_handle_t *task) {
    a20_path_open_args_t image = {.dir = A20_HANDLE_NULL,
                                  .path = (uint64_t)(uintptr_t)supervisor,
                                  .path_len = sizeof(supervisor) - 1,
                                  .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    if (a20_path_open(&image) < 0)
        return -1;
    const char *argv[] = {supervisor, path, 0};
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .image = image.out_handle,
                                  .root_dir = A20_HANDLE_NULL,
                                  .cwd_dir = A20_HANDLE_NULL,
                                  .event_queue = A20_HANDLE_NULL,
                                  .stdin_handle = A20_HANDLE_NULL,
                                  .stdout_handle = output,
                                  .stderr_handle = output,
                                  .argv = (uint64_t)(uintptr_t)argv,
                                  .argc = 2};
    int r = a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0);
    a20_hdl_close(image.out_handle);
    if (r < 0)
        return -1;
    *task = args.out_task;
    return 0;
}
static int reject_spawn_flags(void) {
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .image = A20_HANDLE_NULL,
                                  .flags = 1u << 31};
    if (a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return -1;
    args.flags = 0;
    args.reserved = 1;
    return a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0) ==
                   -A20_ERR_INVALID_ARGUMENT
               ? 0
               : -1;
}
static int wait_exit(a20_handle_t task, unsigned code, unsigned budget) {
    uint64_t start = a20_agent_clock_ms(0);
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    int r;
    do {
        r = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (r != -A20_ERR_WOULD_BLOCK)
            break;
        sleep_ms(1);
    } while (a20_agent_clock_ms(0) - start < budget);
    if (r < 0) {
        a20_task_kill(task, 9);
        return -1;
    }
    a20_hdl_close(task);
    return (unsigned)status.exit_code == code ? 0 : -1;
}
static int status(uint8_t reply[128]) {
    a20_node_id_t self = {{0}};
    a20_cluster_snapshot_args_t snapshot = {0};
    if (a20_cluster_snapshot(&snapshot) < 0)
        return -1;
    /* Zero node ID selects the Native local fast path. */
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect(&self, 0, "cluster.supervisor", 0, 0, &ep) < 0)
        return -1;
    const uint8_t request[8] = {'C', 'S', 1};
    uint32_t len = 128;
    int r =
        a20_cluster_exchange(ep, request, sizeof(request), reply, &len, 100);
    a20_hdl_close(ep);
    return r < 0 || len != 128 || reply[0] != 'C' || reply[1] != 'S' ||
                   reply[2] != 1 || reply[3] || a20_work_u32(reply + 4) != 3
               ? -1
               : 0;
}
static int running(unsigned crashes) {
    uint8_t reply[128];
    uint64_t started = a20_agent_clock_ms(0);
    do {
        if (!status(reply)) {
            int ready = 1;
            for (unsigned i = 0; i < 3; i++) {
                uint8_t *row = reply + 8 + i * 40;
                if (i == 0 && crashes) {
                    ready &= a20_work_u64(row + 16) >= crashes &&
                             a20_work_u32(row + 32) == 42;
                } else
                    ready &= a20_work_u32(row) == 3 &&
                             a20_work_u64(row + 8) == 1 &&
                             !a20_work_u64(row + 16);
            }
            if (ready)
                return 0;
        }
        sleep_ms(5);
    } while (a20_agent_clock_ms(0) - started < 5000);
    return -1;
}
static int call_work(void) {
    a20_cluster_selection_t selected;
    uint64_t started = a20_agent_clock_ms(0);
    do {
        if (!a20_cluster_resolve_any("work", A20_WORK_OPERATOR, A20_WORK_DOT,
                                     12, 200, &selected))
            break;
        sleep_ms(10);
    } while (a20_agent_clock_ms(0) - started < 5000);
    if (a20_agent_clock_ms(0) - started >= 5000)
        return -1;
    a20_handle_t ep = A20_HANDLE_NULL;
    a20_node_id_t local = {{0}};
    if (a20_cluster_connect_bound(&local, selected.slot, selected.boot_epoch,
                                  &ep) < 0)
        return -1;
    uint8_t input[12], wire[44], reply[40];
    a20_work_put32(input, 1);
    a20_work_put32(input + 4, 7);
    a20_work_put32(input + 8, 6);
    a20_work_request_t q = {.job = 1,
                            .shard = 1,
                            .attempt = 1,
                            .kind = A20_WORK_OPERATOR,
                            .operator_id = A20_WORK_DOT,
                            .budget_ms = 1000,
                            .input = input,
                            .input_len = sizeof(input)};
    int n = a20_work_encode(wire, sizeof(wire), &q);
    uint32_t len = sizeof(reply);
    int r = a20_cluster_exchange(ep, wire, n, reply, &len, 1000);
    a20_hdl_close(ep);
    a20_work_result_t result;
    return r < 0 || a20_work_result_decode(reply, len, &q, &result) < 0 ||
                   result.status || result.output_len != 8 ||
                   a20_work_u64(result.output) != 42
               ? -1
               : 0;
}
static int quiescent(void) {
    a20_cluster_resource_args_t r;
    uint64_t started = a20_agent_clock_ms(0);
    do {
        if (!a20_cluster_resources(&r) && !r.values[A20_CLX_STAT_EXPORTS] &&
            !r.values[A20_CLX_STAT_PROXIES] && !r.values[A20_CLX_STAT_INFLIGHT])
            return 0;
        sleep_ms(1);
    } while (a20_agent_clock_ms(0) - started < 1000);
    return -1;
}
static int task_stat(const char *key, uint64_t *value) {
    static const char path[] = "/proc/a20/task_lifetime";
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = sizeof(path) - 1,
                                 .rights = A20_RIGHT_READ};
    if (a20_path_open(&file) < 0)
        return -1;
    char bytes[2048];
    uint64_t got = 0;
    int r = a20_hdl_read_buf(file.out_handle, bytes, sizeof(bytes) - 1, &got);
    a20_hdl_close(file.out_handle);
    if (r < 0 || got >= sizeof(bytes))
        return -1;
    bytes[got] = 0;
    unsigned n = a20_strlen(key);
    for (char *p = bytes; *p;) {
        char *end = p;
        while (*end && *end != '\n')
            end++;
        if ((unsigned)(end - p) > n + 2 && !a20_memcmp(p, key, n) &&
            p[n] == ':' && p[n + 1] == ' ') {
            *value = 0;
            for (char *d = p + n + 2; d < end; d++) {
                if (*d < '0' || *d > '9')
                    return -1;
                *value = *value * 10 + (unsigned)(*d - '0');
            }
            return 0;
        }
        p = *end ? end + 1 : end;
    }
    return -1;
}
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    a20_start_info_t *start = a20_get_start_info();
    if (start)
        output = start->stdout_handle;
    if (reject_spawn_flags())
        return fail("spawn flags");
    a20_handle_t task = A20_HANDLE_NULL;
    if (write_config("version 1\nnode invalid\nrole worker\n") ||
        spawn(&task) || wait_exit(task, 1, 1000))
        return fail("invalid config");
    a20_cluster_snapshot_args_t self = {0};
    if (a20_cluster_snapshot(&self) >= 0)
        return fail("invalid config changed identity");
    a20_runtime_info_t initial;
    if (a20_runtime_info(&initial) || !a20_runtime_info_valid(&initial) ||
        initial.state)
        return fail("unexpected runtime binding");
    const char normal[] =
        "version 1\nnode 00000000000000000000000000000001\ninstance "
        "a2200000000000000000000000000001\nepoch 72623859790382856\nrole "
        "worker\ntiming-scale 10\nseed 00000000000000000000000000000003 "
        "udp:10.0.3.3:44020\nservice agent /bin/cluster-agent-rv\nservice "
        "control "
        "/bin/clusterd-rv\nservice jobs /bin/jobd-rv\nprocess "
        "/bin/cluster-task-probe-rv\n";
    if (write_config(normal) || spawn(&task) || running(0) || call_work())
        return fail("configured services");
    a20_runtime_info_t bound;
    if (a20_runtime_info(&bound) || !a20_runtime_info_valid(&bound) ||
        bound.state != A20_RUNTIME_BOUND || bound.instance_id[0] != 0xa2 ||
        bound.instance_id[1] != 0x20 || bound.instance_id[15] != 1 ||
        bound.domain_id[15] != 1 || bound.boot_epoch != 0x0102030405060708ULL)
        return fail("runtime bind");
    char conflict[sizeof(normal)];
    a20_memcpy(conflict, normal, sizeof(normal));
    for (unsigned i = 0; i + 5 < sizeof(conflict); i++)
        if (!a20_memcmp(conflict + i, "epoch", 5)) {
            conflict[i + 6] = '8';
            break;
        }
    a20_handle_t rejected;
    if (write_config(conflict) || spawn(&rejected) ||
        wait_exit(rejected, 6, 1000) || running(0) || call_work() ||
        write_config(normal))
        return fail("runtime conflicting deployment");
    a20_handle_t duplicate;
    if (spawn(&duplicate) || wait_exit(duplicate, 4, 1000) || running(0) ||
        call_work())
        return fail("singleton");
    if (a20_task_kill(task, 15) < 0 || wait_exit(task, 0, 3000) || quiescent())
        return fail("shutdown");
    uint64_t baseline, baseline_zombies, baseline_errors;
    sleep_ms(100);
    if (task_stat("task_objects", &baseline) ||
        task_stat("zombies", &baseline_zombies) ||
        task_stat("lifetime_errors", &baseline_errors))
        return fail("lifetime baseline");
    const char crash[] =
        "version 1\nnode 00000000000000000000000000000001\ninstance "
        "a2200000000000000000000000000001\nepoch 72623859790382856\nrole "
        "worker\nservice agent /bin/cluster-task-probe-rv\nservice control "
        "/bin/clusterd-rv\nservice jobs /bin/jobd-rv\n";
    if (write_config(crash) || spawn(&task) || running(3))
        return fail("restart backoff");
    if (a20_task_kill(task, 15) < 0 || wait_exit(task, 0, 3000) || quiescent())
        return fail("restart cleanup");
    if (write_config(normal) || spawn(&task) || running(0) || call_work())
        return fail("config recovery");
    if (a20_task_kill(task, 9) < 0 || wait_exit(task, 137, 3000) || quiescent())
        return fail("owner death cleanup");
    if (spawn(&task) || running(0) || call_work())
        return fail("owner death recovery");
    if (a20_task_kill(task, 15) < 0 || wait_exit(task, 0, 3000) || quiescent())
        return fail("final cleanup");
    a20_runtime_info_t recovered;
    if (a20_runtime_info(&recovered) ||
        a20_memcmp(&bound, &recovered, sizeof(bound)))
        return fail("runtime identity changed on supervisor recovery");
    note("SUPERVISOR: runtime immutable/conflict/restart PASS\n");
    uint64_t tasks = UINT64_MAX, zombies = UINT64_MAX, errors = UINT64_MAX;
    uint64_t waited = a20_agent_clock_ms(0);
    do {
        sleep_ms(10);
        if (task_stat("task_objects", &tasks) ||
            task_stat("zombies", &zombies) ||
            task_stat("lifetime_errors", &errors))
            return fail("lifetime read");
        if (tasks <= baseline && zombies <= baseline_zombies)
            break;
    } while (a20_agent_clock_ms(0) - waited < 2000);
    if (tasks > baseline || zombies > baseline_zombies ||
        errors != baseline_errors)
        return fail("lifetime release");
    note("SUPERVISOR: persisted config/singleton/crash "
         "restart/recovery/cleanup PASS\n");
    return 0;
}
