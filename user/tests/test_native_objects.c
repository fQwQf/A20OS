/* Mixed-ISA actual-kernel object publication and leased directory lifecycle. */
#include "liba20rt/a20_objects.h"
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
static void number(uint64_t value);
static int spawn(const char *path, const char **argv, unsigned argc,
                 a20_handle_t *task) {
    a20_path_open_args_t file = {.dir = A20_HANDLE_NULL,
                                 .path = (uint64_t)(uintptr_t)path,
                                 .path_len = (uint32_t)a20_strlen(path),
                                 .rights = A20_RIGHT_READ | A20_RIGHT_EXEC};
    a20_status_t opened = a20_path_open(&file);
    if (opened < 0) {
        note("DIAG open ");
        note(path);
        note(" ");
        number(opened);
        return -1;
    }
    a20_task_spawn_args_t args = {0};
    args.size = sizeof(args);
    args.version = 2;
    args.image = file.out_handle;
    args.root_dir = args.cwd_dir = args.event_queue = A20_HANDLE_NULL;
    args.stdin_handle = A20_HANDLE_NULL;
    args.stdout_handle = args.stderr_handle = output;
    args.argv = (uint64_t)(uintptr_t)argv;
    args.argc = argc;
    a20_status_t created =
        a20_syscall6(A20_SYS_task_spawn, (uint64_t)&args, 0, 0, 0, 0, 0);
    int ok = created >= 0;
    if (!ok) {
        note("DIAG spawn ");
        note(path);
        note(" ");
        number(created);
    }
    a20_hdl_close(file.out_handle);
    if (ok)
        *task = args.out_task;
    return ok ? 0 : -1;
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

static a20_handle_t daemon = A20_HANDLE_NULL, child = A20_HANDLE_NULL;
static a20_object_publication_t publication = {.endpoint = A20_HANDLE_NULL};
static a20_object_id_t current_id;
static int renew_enabled = 1;
static void number(uint64_t value) {
    char text[18] = "0000000000000000\n";
    for (unsigned i = 0; i < 16; i++)
        text[15 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15];
    note(text);
}
static int start_daemon(void) {
    const char *argv[] = {"/bin/objectd-" A20_CLUSTER_ARCH_TAG, 0};
    if (spawn(argv[0], argv, 1, &daemon))
        return -1;
    for (unsigned i = 0; i < 2000; i++) {
        a20_handle_t ep = A20_HANDLE_NULL;
        a20_status_t status =
            a20_cluster_connect(0, 0, A20_OBJECT_SERVICE, 0, 0, &ep);
        if (i == 1999) {
            note("DIAG daemon connect ");
            number(status);
        }
        if (status >= 0) {
            a20_hdl_close(ep);
            return 0;
        }
        sleep_ms(10);
    }
    return -1;
}
static int publish_child(void) {
    a20_object_close(&publication);
    a20_status_t status = a20_object_publish(child, 30000, 4000, &publication);
    if (status < 0) {
        note("DIAG publish ");
        number(status);
        return -1;
    }
    current_id = publication.id;
    renew_enabled = 1;
    return 0;
}
static int new_child(void) {
    const char *argv[] = {"/bin/native-objects-" A20_CLUSTER_ARCH_TAG, "child",
                          0};
    if (spawn(argv[0], argv, 2, &child)) {
        note("DIAG spawn child fail\n");
        return -1;
    }
    return publish_child();
}
static int command(void *ctx, const uint8_t *q, uint32_t n, uint8_t *reply,
                   uint32_t cap) {
    (void)ctx;
    if (n != 4 || cap < 72 || q[0] != 'O' || q[1] != 'T' || q[2] != 1)
        return -1;
    int status = 0;
    if (q[3] == 2) {
        stop(child);
        child = A20_HANDLE_NULL;
    } else if (q[3] == 3) {
        status = a20_object_revoke(&publication, 4000) < 0;
        renew_enabled = 0;
    } else if (q[3] == 4)
        status = new_child();
    else if (q[3] == 5) {
        stop(daemon);
        daemon = A20_HANDLE_NULL;
        a20_object_close(&publication);
        renew_enabled = 0;
        status = start_daemon();
    } else if (q[3] == 6)
        status = publish_child();
    else if (q[3] == 7) {
        status = a20_object_renew(&publication, 1000, 4000) < 0;
        renew_enabled = 0;
    } else if (q[3] != 1)
        return -1;
    for (unsigned i = 0; i < 72; i++)
        reply[i] = 0;
    reply[0] = 'O';
    reply[1] = 'T';
    reply[2] = 1;
    reply[3] = q[3];
    a20_work_put32(reply + 4, status ? 1 : 0);
    a20_object_id_encode(reply + 8, 64, &current_id);
    return 72;
}
static int peer_once(unsigned who, unsigned op, a20_object_id_t *id) {
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[who], &node);
    uint32_t slot;
    uint64_t epoch;
    if (a20_cluster_resolve(&node, "objects.test", 12, 8000, &slot, &epoch) < 0)
        return -1;
    a20_handle_t ep = A20_HANDLE_NULL;
    if (a20_cluster_connect_bound(&node, slot, epoch, &ep) < 0)
        return -1;
    uint8_t q[4] = {'O', 'T', 1, (uint8_t)op}, reply[72];
    uint32_t len = 72;
    int ok = a20_cluster_exchange(ep, q, 4, reply, &len, 8000) >= 0 &&
             len == 72 && !a20_memcmp(q, reply, 4) &&
             !a20_work_u32(reply + 4) &&
             !a20_object_id_decode(reply + 8, 64, id);
    a20_hdl_close(ep);
    return ok ? 0 : -1;
}
static int peer(unsigned who, unsigned op, a20_object_id_t *id) {
    if (op != 1)
        return peer_once(who, op, id);
    uint64_t start = a20_agent_clock_ms(0);
    do {
        if (!peer_once(who, op, id))
            return 0;
        sleep_ms(100);
    } while (a20_agent_clock_ms(0) - start < 20000);
    return -1;
}
static int fail(const char *step) {
    note("OBJECTS: FAIL ");
    note(step);
    note("\n");
    return 1;
}
static int is_state(a20_object_reference_t *ref, unsigned state) {
    a20_object_snapshot_t s;
    return a20_object_query(ref, 4000, &s) >= 0 && s.state == state ? 0 : -1;
}
/* Same kernel accounting used by the application fault matrix: a directory
 * death and a lease/revoke must not leave a task object, zombie or proxy. */
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
static uint64_t base_tasks, base_zombies, base_errors;
static int capture_resource_baseline(void) {
    return task_stat("task_objects", &base_tasks) ||
                   task_stat("zombies", &base_zombies) ||
                   task_stat("lifetime_errors", &base_errors)
               ? -1
               : 0;
}
static int link_resources_empty(void) {
    a20_cluster_resource_args_t r = {0}, q = {0};
    if (a20_cluster_resources(&r) < 0 ||
        a20_cluster_reassembly_resources(&q) < 0)
        return 0;
    return !r.values[A20_CLX_STAT_EXPORTS] && !r.values[A20_CLX_STAT_PROXIES] &&
           !r.values[A20_CLX_STAT_INFLIGHT] &&
           !r.values[A20_CLX_STAT_INFLIGHT_BYTES] &&
           !q.values[A20_CLX_REASM_CONTEXTS] && !q.values[A20_CLX_REASM_BYTES];
}
static int resources_recovered(unsigned seconds) {
    uint64_t start = a20_agent_clock_ms(0);
    for (;;) {
        uint64_t tasks = 0, zombies = 0, errors = 0;
        if (!task_stat("task_objects", &tasks) &&
            !task_stat("zombies", &zombies) &&
            !task_stat("lifetime_errors", &errors) && tasks == base_tasks &&
            zombies == base_zombies && errors == base_errors &&
            link_resources_empty())
            return 0;
        if (a20_agent_clock_ms(0) - start >= (uint64_t)seconds * 1000)
            return -1;
        sleep_ms(20);
    }
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    if (argc == 2 && !a20_strcmp(argv[1], "child"))
        for (;;)
            sleep_ms(1000);
    if (argc != 3 || argv[1][0] < '0' || argv[1][0] > '2')
        return 1;
    role = argv[1][0] - '0';
    unsigned mode = argv[2][0] - '0';
    a20_node_id_t self;
    a20_node_id_parse_hex(identities[role], &self);
    if (a20_cluster_set_self(&self, 0) < 0)
        return fail("self");
    a20_runtime_binding_t binding = {
        .size = 64,
        .version = 1,
        .instance_id = {0x88},
        .boot_epoch = role == 1 && mode ? 201 : 100 * (role + 1)};
    a20_memcpy(binding.domain_id, self.bytes, 16);
    if (a20_runtime_bind(&binding) < 0)
        return fail("bind");
    for (unsigned i = 0; i < 3; i++)
        if (i != role) {
            a20_cluster_route_args_t route = {.op = A20_ROUTE_ADD,
                                              .transport_id =
                                                  A20_CLX_TRANSPORT_UDP,
                                              .next_hop_len = 6};
            a20_node_id_parse_hex(identities[i], &route.node_id);
            a20_cluster_parse_udp(addresses[i], route.next_hop);
            if (a20_cluster_route(A20_ROUTE_ADD, &route.node_id,
                                  A20_CLX_TRANSPORT_UDP, route.next_hop, 6,
                                  0) < 0)
                return fail("route");
        }
    if (!role && capture_resource_baseline())
        return fail("task lifetime baseline");
    if (start_daemon())
        return fail("start daemon");
    if (new_child())
        return fail("publish child");
    if (role) {
        a20_channel_pair_t pair;
        if (a20_channel_create(&pair) < 0 ||
            a20_cluster_export(pair.endpoints[0], "objects.test", 0, 0) < 0)
            return fail("test export");
        a20_hdl_close(pair.endpoints[1]);
        a20_work_agent_t unused = {0};
        a20_cluster_agent_server_t server;
        a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
        server.dispatch = command;
        note("OBJECTS: peer-ready\n");
        uint64_t renew_at = a20_agent_clock_ms(0) + 3000;
        for (;;) {
            if (a20_cluster_agent_pump(&server) < 0)
                return fail("test pump");
            if (renew_enabled && a20_agent_clock_ms(0) >= renew_at) {
                if (a20_object_renew(&publication, 10000, 4000) < 0)
                    return fail("peer renew");
                renew_at = a20_agent_clock_ms(0) + 3000;
            }
            a20_task_status_t status = {.size = sizeof(status), .version = 1};
            if (a20_task_wait(daemon, A20_TASK_WAIT_NONBLOCK, &status) >= 0)
                return fail("daemon exited");
            sleep_ms(1);
        }
    }
    a20_object_reference_t b = {.endpoint = A20_HANDLE_NULL},
                           c = {.endpoint = A20_HANDLE_NULL},
                           local = {.endpoint = A20_HANDLE_NULL};
    a20_object_snapshot_t snapshot;
    if (a20_object_open(&current_id, 4000, &local, &snapshot) < 0 ||
        snapshot.state != 1)
        return fail("local open");
    a20_object_id_t bid, cid, newid;
    if (peer(1, 1, &bid) || peer(2, 1, &cid))
        return fail("peer ID");
    if (a20_object_open(&bid, 8000, &b, &snapshot) < 0 || snapshot.state != 1 ||
        a20_object_open(&cid, 8000, &c, &snapshot) < 0 || snapshot.state != 1)
        return fail("mixed open");
    a20_object_id_t bad = bid;
    bad.instance_id[0] ^= 1;
    a20_object_reference_t denied = {.endpoint = A20_HANDLE_NULL};
    if (a20_object_open(&bad, 4000, &denied, &snapshot) != -A20_ERR_ACCESS)
        return fail("foreign instance");
    if (peer(1, 2, &newid) || is_state(&b, 2))
        return fail("retired query");
    note("OBJECTS: retired-pass\n");
    if (peer(1, 3, &newid) ||
        a20_object_query(&b, 4000, &snapshot) != -A20_ERR_NOT_FOUND ||
        b.endpoint != A20_HANDLE_NULL)
        return fail("revoke");
    note("OBJECTS: revoke-pass\n");
    /* The fault phases below extend the run past one 30 s lease interval. */
    if (a20_object_renew(&publication, 30000, 4000) < 0)
        return fail("renew after revoke");
    if (peer(1, 4, &newid) || a20_object_id_equal(&newid, &bid) ||
        a20_object_open(&newid, 8000, &b, &snapshot) < 0)
        return fail("new task identity");
    bid = newid;
    if (peer(2, 7, &newid))
        return fail("expiry setup");
    sleep_ms(2000);
    if (a20_object_query(&c, 4000, &snapshot) != -A20_ERR_NOT_FOUND)
        return fail("lease expiration");
    note("OBJECTS: expiry-pass\n");
    if (peer(2, 6, &newid) || !a20_object_id_equal(&newid, &cid) ||
        a20_object_open(&cid, 8000, &c, &snapshot) < 0)
        return fail("republication");
    /* Unreachable is not RETIRED: the owner shard keeps the record and the
     * same object id resolves LIVE after the link comes back. */
    note("OBJECTS: partition-ready\n");
    sleep_ms(500); /* let the host apply the link break before the query */
    if (a20_object_query(&c, 1500, &snapshot) >= 0)
        return fail("partition query");
    sleep_ms(4500);
    if (a20_object_open(&cid, 8000, &c, &snapshot) < 0 || snapshot.state != 1)
        return fail("partition recovery");
    note("OBJECTS: partition-pass\n");
    if (a20_object_renew(&publication, 30000, 4000) < 0)
        return fail("renew after partition");
    if (peer(1, 5, &newid))
        return fail("daemon restart");
    if (a20_object_query(&b, 4000, &snapshot) >= 0 ||
        b.endpoint != A20_HANDLE_NULL)
        return fail("old proxy");
    if (a20_object_open(&bid, 8000, &b, &snapshot) != -A20_ERR_NOT_FOUND)
        return fail("restart empty");
    if (peer(1, 6, &newid) || !a20_object_id_equal(&newid, &bid) ||
        a20_object_open(&bid, 8000, &b, &snapshot) < 0)
        return fail("restart reexport");
    note("OBJECTS: service-death-pass\n");
    if (a20_object_renew(&publication, 30000, 4000) < 0)
        return fail("local renewal");
    note("OBJECTS: lifecycle-pass\nOBJECTS: reboot-ready\n");
    sleep_ms(11000);
    if (peer(1, 1, &newid) || newid.boot_epoch != 201 ||
        a20_object_id_equal(&newid, &bid))
        return fail("reboot identity");
    if (a20_object_query(&b, 4000, &snapshot) >= 0)
        return fail("old reboot proxy");
    if (a20_object_open(&bid, 8000, &b, &snapshot) != -A20_ERR_EXPIRED ||
        a20_object_open(&newid, 8000, &b, &snapshot) < 0)
        return fail("reboot resolve");
    if (a20_object_revoke(&publication, 4000) < 0 ||
        a20_object_query(&local, 4000, &snapshot) != -A20_ERR_NOT_FOUND)
        return fail("local revoke");
    a20_object_close(&b);
    a20_object_close(&c);
    a20_object_close(&local);
    stop(child);
    stop(daemon);
    if (resources_recovered(15))
        return fail("final resource recovery");
    note("OBJECTS: recovered-pass tasks/zombies/errors=baseline "
         "cluster=empty\n");
    note("OBJECTS: PASS\n");
    return 0;
}
