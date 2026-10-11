/* Persisted cluster deployment owner. Only local configured executables are
 * spawned. Child exit/reap and bounded backoff never block the other services. */
#include "liba20rt/a20_cluster_agent.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_config.h"

static a20_cluster_config_t config;
typedef struct child {
    a20_handle_t image, task;
    a20_restart_policy_t policy;
    int32_t last_exit;
    uint32_t reason, pid;
} child_t;
static child_t children[A20_CONFIG_SERVICES];
static char identity[33];
static a20_handle_t output = A20_HANDLE_NULL;
static void note(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static int open_path(const char *path, uint32_t rights, a20_handle_t *out) {
    a20_path_open_args_t args = {.dir = A20_HANDLE_NULL,
                                 .rights = rights,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path)};
    if (a20_path_open(&args) < 0)
        return -1;
    *out = args.out_handle;
    return 0;
}
static int load_config(const char *path) {
    a20_handle_t file;
    static char bytes[A20_CONFIG_BYTES + 1];
    if (open_path(path, A20_RIGHT_READ, &file))
        return -1;
    unsigned used = 0;
    int result = -1;
    while (used < sizeof(bytes)) {
        uint64_t got = 0;
        if (a20_hdl_read_buf(file, bytes + used, sizeof(bytes) - used, &got) <
                0 ||
            got > sizeof(bytes) - used)
            break;
        if (!got) {
            result = a20_cluster_config_parse(bytes, used, &config, 0);
            break;
        }
        used += got;
    }
    a20_hdl_close(file);
    return result;
}
static int seed_exists(const a20_config_seed_t *seed) {
    a20_cluster_snapshot_args_t route = {.kind = A20_CLUSTER_SNAPSHOT_ROUTE};
    for (unsigned i = 0; i < 8192; i++) {
        int r = a20_cluster_snapshot(&route);
        if (r == -A20_ERR_NOT_FOUND)
            return 0;
        if (r == -A20_ERR_BUSY) {
            route.index = 0;
            route.generation = 0;
            continue;
        }
        if (r < 0)
            return -1;
        if (!a20_memcmp(route.node_id, seed->node, 16))
            return route.transport_id == seed->transport &&
                           route.next_hop_len == seed->hop_len &&
                           !a20_memcmp(route.next_hop, seed->hop,
                                       seed->hop_len) &&
                           !route.metric
                       ? 1
                       : -1;
        route.index = route.next_index;
    }
    return -1;
}
static int install_seeds(void) {
    uint8_t added[A20_CONFIG_SEEDS] = {0};
    unsigned i;
    for (i = 0; i < config.seed_count; i++) {
        const a20_config_seed_t *s = &config.seeds[i];
        int exists = seed_exists(s);
        if (exists < 0)
            break;
        if (exists)
            continue;
        a20_node_id_t id;
        a20_memcpy(id.bytes, s->node, 16);
        if (a20_cluster_route(A20_ROUTE_ADD, &id, s->transport, s->hop,
                              s->hop_len, 0) < 0)
            break;
        added[i] = 1;
    }
    if (i == config.seed_count)
        return 0;
    for (unsigned j = 0; j < i; j++)
        if (added[j]) {
            const a20_config_seed_t *s = &config.seeds[j];
            a20_node_id_t id;
            a20_memcpy(id.bytes, s->node, 16);
            a20_cluster_route(A20_ROUTE_DEL_MATCH, &id, s->transport, s->hop,
                              s->hop_len, 0);
        }
    return -1;
}
static void start_child(unsigned i, uint64_t now) {
    child_t *c = &children[i];
    if (c->policy.starts == UINT64_MAX) {
        c->policy.exhausted = 1;
        return;
    }
    const char *argv[4] = {config.service[i], 0, 0, 0};
    unsigned argc = 1;
    if (i == A20_CONFIG_AGENT && config.process[0]) {
        argv[1] = identity;
        argv[2] = config.process;
        argc = 3;
    }
    a20_task_spawn_args_t args = {.size = sizeof(args),
                                  .version = 2,
                                  .image = c->image,
                                  .root_dir = A20_HANDLE_NULL,
                                  .cwd_dir = A20_HANDLE_NULL,
                                  .event_queue = A20_HANDLE_NULL,
                                  .stdin_handle = A20_HANDLE_NULL,
                                  .stdout_handle = output,
                                  .stderr_handle = output,
                                  .argv = (uint64_t)(uintptr_t)argv,
                                  .argc = argc,
                                  .flags = A20_SPAWN_PARENT_BOUND};
    c->policy.starts++;
    c->policy.started = now;
    int r = a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r < 0) {
        c->last_exit = r;
        c->reason = 0;
        a20_restart_failed(&c->policy, now);
        return;
    }
    c->task = args.out_task;
    a20_task_info_t info = {.size = sizeof(info), .version = 1};
    if (!a20_task_info(c->task, &info))
        c->pid = info.pid;
}
static int reap_child(child_t *c, uint64_t now, int restart) {
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    int r = a20_task_wait(c->task, A20_TASK_WAIT_NONBLOCK, &status);
    if (r == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (r < 0)
        return -1; /* Never launch a replacement while ownership is unknown. */
    c->last_exit = status.exit_code;
    c->reason = status.exit_reason;
    a20_hdl_close(c->task);
    c->task = A20_HANDLE_NULL;
    c->pid = 0;
    if (restart)
        a20_restart_failed(&c->policy, now);
    return 1;
}
/* Read-only local status: CS/1 request 8 B, reply 128 B with 3 x 40 B rows.
 * Each row: enabled/running/exhausted flags, pid, starts, exits, next-start-ms,
 * last-exit and exit-reason. No handles or remote administration are exposed. */
static int status_dispatch(void *ctx, const uint8_t *q, uint32_t len,
                           uint8_t *reply, uint32_t cap) {
    (void)ctx;
    if (cap < 128 || len != 8 || q[0] != 'C' || q[1] != 'S' || q[2] != 1 ||
        q[3] || a20_work_u32(q + 4))
        return -1;
    a20_memset(reply, 0, 128);
    reply[0] = 'C';
    reply[1] = 'S';
    reply[2] = 1;
    a20_work_put32(reply + 4, A20_CONFIG_SERVICES);
    for (unsigned i = 0; i < A20_CONFIG_SERVICES; i++) {
        child_t *c = &children[i];
        uint8_t *p = reply + 8 + i * 40;
        a20_work_put32(p, (c->image != A20_HANDLE_NULL ? 1u : 0u) |
                              (c->task != A20_HANDLE_NULL ? 2u : 0u) |
                              (c->policy.exhausted ? 4u : 0u));
        a20_work_put32(p + 4, c->pid);
        a20_work_put64(p + 8, c->policy.starts);
        a20_work_put64(p + 16, c->policy.exits);
        a20_work_put64(p + 24, c->policy.next_start);
        a20_work_put32(p + 32, (uint32_t)c->last_exit);
        a20_work_put32(p + 36, c->reason);
    }
    return 128;
}
static int shutdown_children(void) {
    for (unsigned i = 0; i < A20_CONFIG_SERVICES; i++)
        if (children[i].task != A20_HANDLE_NULL)
            a20_task_kill(children[i].task, 15);
    uint64_t started = a20_agent_clock_ms(0);
    for (;;) {
        unsigned active = 0;
        uint64_t now = a20_agent_clock_ms(0);
        for (unsigned i = 0; i < A20_CONFIG_SERVICES; i++) {
            child_t *c = &children[i];
            if (c->task == A20_HANDLE_NULL)
                continue;
            if (reap_child(c, now, 0) < 0)
                return -1;
            if (c->task != A20_HANDLE_NULL) {
                active++;
                if (now - started >= 1000)
                    a20_task_kill(c->task, 9);
            }
        }
        if (!active)
            return 0;
        if (now - started >= 2000)
            return -1;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *start = a20_get_start_info();
    if (start)
        output = start->stdout_handle;
    if (argc != 2 || load_config(argv[1])) {
        note("CLUSTER_SUPERVISOR: invalid config\n");
        return 1;
    }
    for (unsigned i = 0; i < A20_CONFIG_SERVICES; i++) {
        children[i].image = children[i].task = A20_HANDLE_NULL;
        if (config.service[i][0] &&
            open_path(config.service[i], A20_RIGHT_READ | A20_RIGHT_EXEC,
                      &children[i].image))
            return 2;
    }
    if (config.process[0]) {
        a20_handle_t image;
        if (open_path(config.process, A20_RIGHT_READ | A20_RIGHT_EXEC, &image))
            return 2;
        a20_hdl_close(image);
    }
    a20_node_id_t id;
    a20_memcpy(id.bytes, config.node, 16);
    for (unsigned i = 0; i < 16; i++) {
        identity[2 * i] = "0123456789abcdef"[id.bytes[i] >> 4];
        identity[2 * i + 1] = "0123456789abcdef"[id.bytes[i] & 15];
    }
    a20_cluster_snapshot_args_t self = {0};
    if (a20_cluster_snapshot(&self) < 0) {
        if (a20_cluster_set_self(&id, config.caps) < 0)
            return 3;
    } else if (a20_memcmp(self.node_id, config.node, 16) ||
               self.caps != config.caps)
        return 3;
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], "cluster.supervisor",
                           A20_EXPORT_LOCAL_ONLY, 0) < 0)
        return 4;
    a20_hdl_close(pair.endpoints[1]);
    if (install_seeds())
        return 5;
    a20_cluster_agent_server_t server;
    a20_work_agent_t unused = {0};
    a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
    server.dispatch = status_dispatch;
    note("CLUSTER_SUPERVISOR: ready\n");
    int result = 0;
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            break;
        uint64_t now = a20_agent_clock_ms(0);
        for (unsigned i = 0; i < A20_CONFIG_SERVICES; i++) {
            child_t *c = &children[i];
            if (c->task != A20_HANDLE_NULL) {
                if (reap_child(c, now, 1) < 0) {
                    result = 6;
                    goto stop;
                }
            } else if (c->image != A20_HANDLE_NULL && !c->policy.exhausted &&
                       now >= c->policy.next_start)
                start_child(i, now);
        }
        if (a20_cluster_agent_pump(&server) < 0) {
            result = 7;
            break;
        }
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
stop:
    if (shutdown_children())
        result = 8;
    note(result ? "CLUSTER_SUPERVISOR: cleanup failed\n"
                : "CLUSTER_SUPERVISOR: stopped\n");
    return result;
}
