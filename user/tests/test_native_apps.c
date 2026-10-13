/* Mixed-ISA actual-kernel application owner lifecycle. */
#include "liba20rt/a20_apps.h"
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
    a20_start_info_t *si = a20_get_start_info();
    args.root_dir = si->root_dir;
    args.cwd_dir = si->cwd_dir;
    args.event_queue = A20_HANDLE_NULL;
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

static a20_handle_t daemon = A20_HANDLE_NULL;
static uint8_t local_domain[16];
static void number(uint64_t value) {
    char text[18] = "0000000000000000\n";
    for (unsigned i = 0; i < 16; i++)
        text[15 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15];
    note(text);
}
static const char *loop_id = "c1000000000000000000000000000001";
static const char *exit_id = "c2000000000000000000000000000002";
static const char *bad_id = "c3000000000000000000000000000003";
static const uint8_t loop_uuid[16] = {0xc1, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 1};
static const uint8_t exit_uuid[16] = {0xc2, 0, 0, 0, 0, 0, 0, 0,
                                      0,    0, 0, 0, 0, 0, 0, 2};
static const uint8_t bad_uuid[16] = {0xc3, 0, 0, 0, 0, 0, 0, 0,
                                     0,    0, 0, 0, 0, 0, 0, 3};
static const uint8_t absent_uuid[16] = {0xc4};
static int start_daemon(void) {
    const char *argv[] = {"/bin/appd-" A20_CLUSTER_ARCH_TAG,
                          loop_id,
                          "/bin/native-app-child-rv",
                          "243",
                          "64",
                          "1",
                          "3",
                          "2",
                          loop_id,
                          "/bin/native-app-child-aarch64",
                          "183",
                          "64",
                          "1",
                          "2",
                          "2",
                          loop_id,
                          "/bin/native-app-child-x86_64",
                          "62",
                          "64",
                          "1",
                          "1",
                          "2",
                          exit_id,
                          "/bin/native-app-exit-rv",
                          "243",
                          "64",
                          "1",
                          "3",
                          "2",
                          exit_id,
                          "/bin/native-app-exit-aarch64",
                          "183",
                          "64",
                          "1",
                          "2",
                          "2",
                          exit_id,
                          "/bin/native-app-exit-x86_64",
                          "62",
                          "64",
                          "1",
                          "1",
                          "2",
                          bad_id,
                          "/bin/absent-application",
                          0,
                          "64",
                          "1",
                          0,
                          "2",
                          0};
    a20_execution_info_t exec;
    if (a20_execution_info(&exec) < 0)
        return -1;
    argv[45] = exec.elf_machine == 243   ? "243"
               : exec.elf_machine == 183 ? "183"
                                         : "62";
    argv[48] = exec.calling_abi == 3 ? "3" : exec.calling_abi == 2 ? "2" : "1";
    if (spawn(argv[0], argv, 50, &daemon))
        return -1;
    for (unsigned i = 0; i < 2000; i++) {
        a20_app_session_t s;
        a20_status_t status = a20_app_connect(local_domain, 100, &s);
        if (status >= 0) {
            a20_app_close(&s);
            return 0;
        }
        sleep_ms(10);
    }
    return -1;
}
static int start_configured_daemon(void) {
    static const char path[] = "/tmp/a20-apps.manifest";
    a20_execution_info_t exec;
    if (a20_execution_info(&exec) < 0)
        return -1;
    const char *chunks[] = {
        "A20APPS1\napp c5000000000000000000000000000005 /bin/native-app-exit-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\narg hello world\narg \narg 多架构\nenv A20_TEST=cluster\nenv "
        "A20_EMPTY=\nend\n",
        "app c6000000000000000000000000000006 /bin/native-app-child-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nend\n",
        "app c7000000000000000000000000000007 /bin/native-app-exit-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nend\n",
        "app c8000000000000000000000000000008 /bin/app-exec-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nrequire-feature 4\narg /bin/cluster_linux_child\narg exit\narg "
        "hello world\narg \narg 多架构\nenv A20_TEST=cluster\nenv "
        "A20_EMPTY=\nend\n",
        "app c9000000000000000000000000000009 /bin/app-exec-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nrequire-feature 4\narg /bin/cluster_linux_child\narg hold\narg "
        "hello world\narg \narg 多架构\nenv A20_TEST=cluster\nenv "
        "A20_EMPTY=\nend\n",
        "app ca00000000000000000000000000000a /bin/app-exec-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nrequire-feature 255\narg /bin/cluster_linux_child\narg exit\narg "
        "hello world\narg \narg 多架构\nenv A20_TEST=cluster\nenv "
        "A20_EMPTY=\nend\n",
        "app cb00000000000000000000000000000b /bin/app-exec-",
        A20_CLUSTER_ARCH_TAG,
        " ",
        exec.elf_machine == 243   ? "243"
        : exec.elf_machine == 183 ? "183"
                                  : "62",
        " 64 1 ",
        exec.calling_abi == 3   ? "3"
        : exec.calling_abi == 2 ? "2"
                                : "1",
        " 2\nrequire-feature 4\narg /bin/no-such-linux-application\nenv "
        "A20_TEST=cluster\nenv A20_EMPTY=\nend\n",
    };
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
    for (unsigned i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        uint64_t wrote = 0;
        unsigned len = a20_strlen(chunks[i]);
        if (a20_hdl_write_buf(file.out_handle, chunks[i], len, &wrote) < 0 ||
            wrote != len) {
            a20_hdl_close(file.out_handle);
            return -1;
        }
    }
    a20_hdl_close(file.out_handle);
    stop(daemon);
    daemon = A20_HANDLE_NULL;
    const char *argv[] = {"/bin/appd-" A20_CLUSTER_ARCH_TAG, "--manifest", path,
                          0};
    if (spawn(argv[0], argv, 3, &daemon))
        return -1;
    for (unsigned i = 0; i < 2000; i++) {
        a20_app_session_t session;
        if (a20_app_connect(local_domain, 100, &session) >= 0) {
            a20_app_close(&session);
            return 0;
        }
        sleep_ms(10);
    }
    return -1;
}
static int cli_run(unsigned target) {
    const char *argv[] = {"/bin/appctl-run-" A20_CLUSTER_ARCH_TAG,
                          "c5000000000000000000000000000005",
                          "700",
                          "30000",
                          identities[target],
                          0};
    a20_handle_t task;
    if (spawn(argv[0], argv, 5, &task))
        return -1;
    a20_task_status_t status = {.size = sizeof(status), .version = 1};
    for (unsigned i = 0; i < 4500; i++) {
        a20_status_t result =
            a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (result >= 0) {
            a20_hdl_close(task);
            return status.exit_code == 37 ? 0 : -1;
        }
        if (result != -A20_ERR_WOULD_BLOCK)
            break;
        sleep_ms(10);
    }
    stop(task);
    return -1;
}
static int cli_cancel(void) {
    const char *argv[] = {"/bin/appctl-run-" A20_CLUSTER_ARCH_TAG,
                          "c6000000000000000000000000000006",
                          "701",
                          "30000",
                          identities[1],
                          0};
    a20_handle_t task;
    if (spawn(argv[0], argv, 5, &task))
        return -1;
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[1], &node);
    uint8_t manifest[16] = {0xc6};
    manifest[15] = 6;
    int started = 0;
    for (unsigned i = 0; i < 200; i++) {
        a20_app_session_t session;
        a20_app_result_t offer;
        if (a20_app_connect(node.bytes, 4000, &session) >= 0) {
            int ok = a20_app_offer(&session, manifest, 4000, &offer) >= 0 &&
                     offer.active == 1;
            a20_app_close(&session);
            if (ok) {
                started = 1;
                break;
            }
        }
        sleep_ms(10);
    }
    if (!started || a20_task_kill(task, 15) < 0) {
        stop(task);
        return -1;
    }
    int exited = 0;
    for (unsigned i = 0; i < 4500; i++) {
        a20_task_status_t status = {.size = sizeof(status), .version = 1};
        a20_status_t result =
            a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (result >= 0) {
            exited = status.exit_code == 137;
            break;
        }
        if (result != -A20_ERR_WOULD_BLOCK)
            break;
        sleep_ms(10);
    }
    if (!exited) {
        stop(task);
        return -1;
    }
    a20_hdl_close(task);
    a20_app_session_t session;
    a20_app_result_t offer;
    if (a20_app_connect(node.bytes, 8000, &session) < 0)
        return -1;
    int ok =
        a20_app_offer(&session, manifest, 4000, &offer) >= 0 && !offer.active;
    a20_app_close(&session);
    return ok ? 0 : -1;
}
static int control(void *ctx, const uint8_t *q, uint32_t n, uint8_t *reply,
                   uint32_t cap) {
    (void)ctx;
    if (n != 4 || cap < 4 || q[0] != 'A' || q[1] != 'T' || q[2] != 1 ||
        q[3] > 3 || !q[3])
        return -1;
    if (q[3] == 2) {
        stop(daemon);
        daemon = A20_HANDLE_NULL;
        if (start_daemon())
            return -1;
    }
    if (q[3] == 3 && start_configured_daemon())
        return -1;
    a20_memcpy(reply, q, 4);
    return 4;
}
static int peer(unsigned who, unsigned op) {
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[who], &node);
    uint64_t start = a20_agent_clock_ms(0);
    do {
        uint32_t slot;
        uint64_t epoch;
        a20_handle_t ep = A20_HANDLE_NULL;
        if (a20_cluster_resolve(&node, "apps.test", 9, 8000, &slot, &epoch) >=
                0 &&
            a20_cluster_connect_bound(&node, slot, epoch, &ep) >= 0) {
            uint8_t q[4] = {'A', 'T', 1, (uint8_t)op}, reply[4];
            uint32_t n = 4;
            int ok = a20_cluster_exchange(ep, q, 4, reply, &n, 5000) >= 0 &&
                     n == 4 && !a20_memcmp(q, reply, 4);
            a20_hdl_close(ep);
            if (ok)
                return 0;
            if (op != 1)
                return -1;
        }
        sleep_ms(100);
    } while (a20_agent_clock_ms(0) - start < 20000);
    return -1;
}
static int fail(const char *step) {
    note("APPS: FAIL ");
    note(step);
    note("\n");
    return 1;
}
static int exited(a20_app_session_t *s, a20_app_ticket_t *ticket, int expected,
                  a20_app_result_t *out) {
    for (unsigned i = 0; i < 200; i++) {
        if (a20_app_query(s, ticket, 4000, out) < 0)
            return -1;
        if (out->state == A20_APP_EXITED)
            return expected < 0 || out->exit_code == expected ? 0 : -1;
        sleep_ms(10);
    }
    return -1;
}
static int linux_applications(uint8_t domains[3][16]) {
    uint8_t loop[16] = {0xc9}, short_app[16] = {0xc8};
    uint8_t unavailable[16] = {0xca}, missing[16] = {0xcb};
    loop[15] = 9;
    short_app[15] = 8;
    unavailable[15] = 10;
    missing[15] = 11;
    a20_app_ticket_t tickets[6];
    a20_object_id_t roots[6];
    a20_app_result_t result = {0};
    unsigned placed[3] = {0};
    for (unsigned i = 0; i < 6; i++) {
        if (a20_app_start_any_reusable(domains, 3, loop, 3000 + i, 30000,
                                       &tickets[i], &result) < 0 ||
            result.state != A20_APP_RUNNING) {
            note("APPS-LINUX: failure line/state/active/exit\n");
            number(__LINE__);
            number(result.state);
            number(result.active);
            number(result.exit_code);
            return -1;
        }
        roots[i] = result.root;
        for (unsigned k = 0; k < 3; k++)
            if (a20_runtime_id_equal(result.root.domain_id, domains[k]))
                placed[k]++;
    }
    if (placed[0] != 2 || placed[1] != 2 || placed[2] != 2)
        return -1;
    sleep_ms(3000);
    for (unsigned i = 0; i < 6; i++) {
        a20_app_session_t session = {.endpoint = A20_HANDLE_NULL};
        if (a20_app_resume(&tickets[i], 8000, &session) < 0 ||
            a20_app_query(&session, &tickets[i], 4000, &result) < 0 ||
            result.state != A20_APP_RUNNING || result.active != 2 - i / 3 ||
            !a20_object_id_equal(&roots[i], &result.root) ||
            a20_app_stop(&session, &tickets[i], 9, 4000, &result) < 0 ||
            exited(&session, &tickets[i], 137, &result) ||
            !a20_object_id_equal(&roots[i], &result.root) ||
            a20_app_release(&session, &tickets[i], 4000, &result) < 0) {
            note("APPS-LINUX: failure line/state/active/exit\n");
            number(__LINE__);
            number(result.state);
            number(result.active);
            number(result.exit_code);
            return -1;
        }
        a20_app_close(&session);
    }
    for (unsigned i = 0; i < 3; i++) {
        a20_app_session_t session = {.endpoint = A20_HANDLE_NULL};
        a20_app_ticket_t ticket;
        if (a20_app_connect_reusable(domains[i], 8000, &session) < 0 ||
            a20_app_offer(&session, unavailable, 4000, &result) !=
                -A20_ERR_NOT_SUPPORTED)
            return -1;
        a20_app_close(&session);
        if (a20_app_resume(&tickets[i], 8000, &session) < 0 ||
            a20_app_query(&session, &tickets[i], 4000, &result) !=
                -A20_ERR_EXPIRED) {
            note("APPS-LINUX: failure line/state/active/exit\n");
            number(__LINE__);
            number(result.state);
            number(result.active);
            number(result.exit_code);
            return -1;
        }
        a20_app_close(&session);
        if (a20_app_start_any_reusable(&domains[i], 1, short_app, 3100 + i,
                                       30000, &ticket, &result) < 0 ||
            a20_app_resume(&ticket, 8000, &session) < 0 ||
            exited(&session, &ticket, 41, &result) || result.active ||
            a20_app_release(&session, &ticket, 4000, &result) < 0) {
            note("APPS-LINUX: failure line/state/active/exit\n");
            number(__LINE__);
            number(result.state);
            number(result.active);
            number(result.exit_code);
            return -1;
        }
        a20_app_close(&session);
        if (a20_app_start_any_reusable(&domains[i], 1, missing, 3200 + i, 30000,
                                       &ticket, &result) < 0 ||
            a20_app_resume(&ticket, 8000, &session) < 0 ||
            exited(&session, &ticket, 127, &result) || result.active ||
            a20_app_release(&session, &ticket, 4000, &result) < 0) {
            note("APPS-LINUX: failure line/state/active/exit\n");
            number(__LINE__);
            number(result.state);
            number(result.active);
            number(result.exit_code);
            return -1;
        }
        a20_app_close(&session);
    }
    note("APPS: linux-pass real-programs=6 active=2,2,2 threads=3 "
         "identity=stable stop=137 exit=41 missing=127 capability=filtered\n");
    return 0;
}
static int reusable(const uint8_t domain[16], int lose_reply) {
    uint8_t loop[16] = {0xc6}, short_app[16] = {0xc7};
    loop[15] = 6;
    short_app[15] = 7;
    a20_app_session_t session;
    a20_app_result_t offer, result;
    a20_app_ticket_t held, ticket, retired;
    if (a20_app_connect_reusable(domain, 8000, &session) < 0 ||
        a20_app_offer(&session, loop, 4000, &offer) < 0 || !offer.token ||
        a20_app_start_offered(&session, loop, 900, offer.token, 4000, &held,
                              &result) < 0 ||
        result.state != A20_APP_RUNNING)
        return -1;
    a20_object_id_t root = result.root, controller = session.controller;
    if (a20_app_release(&session, &held, 4000, &result) !=
            -A20_ERR_WOULD_BLOCK ||
        a20_app_resume(&held, 8000, &session) < 0)
        return -1;
    for (unsigned i = 0; i < 16; i++) {
        if (a20_app_offer(&session, short_app, 4000, &offer) < 0 ||
            !offer.token || offer.active != 1 || offer.occupied != 1 ||
            a20_app_start_offered(&session, short_app, 1000 + i, offer.token,
                                  4000, &ticket, &result) < 0 ||
            exited(&session, &ticket, 37, &result) ||
            a20_app_release(&session, &ticket, 4000, &result) < 0 ||
            result.occupied != 1 || result.active != 1)
            return -1;
        if (!i)
            retired = ticket;
        if (a20_app_query(&session, &held, 4000, &result) < 0 ||
            result.state != A20_APP_RUNNING ||
            !a20_object_id_equal(&root, &result.root) ||
            !a20_object_id_equal(&controller, &session.controller))
            return -1;
    }
    if (a20_app_call(&session, &retired, A20_APP_START, 0, 4000, &result) !=
            -A20_ERR_EXPIRED ||
        a20_app_resume(&held, 8000, &session) < 0)
        return -1;
    /* Retiring an offered but unaccepted key fences its delayed first START. */
    if (a20_app_offer(&session, short_app, 4000, &offer) < 0 || !offer.token)
        return -1;
    ticket = (a20_app_ticket_t){.controller = session.controller,
                                .client = session.client,
                                .version = 2,
                                .token = offer.token,
                                .nonce = 1100};
    a20_memcpy(ticket.manifest, short_app, 16);
    if (a20_app_release(&session, &ticket, 4000, &result) < 0 ||
        a20_app_call(&session, &ticket, A20_APP_START, 0, 4000, &result) !=
            -A20_ERR_EXPIRED ||
        a20_app_resume(&held, 8000, &session) < 0)
        return -1;
    if (lose_reply) {
        if (a20_app_offer(&session, loop, 4000, &offer) < 0 || !offer.token ||
            a20_app_start_offered(&session, loop, 1300, offer.token, 1500,
                                  &ticket, &result) != -A20_ERR_TIMED_OUT ||
            a20_app_resume(&ticket, 8000, &session) < 0 ||
            a20_app_query(&session, &ticket, 4000, &result) < 0 ||
            result.state != A20_APP_RUNNING)
            return -1;
        a20_object_id_t pending_root = result.root;
        note("APPS: reusable-start-timeout-pass original-root=live\n");
        if (a20_app_call(&session, &ticket, A20_APP_START, 0, 4000, &result) <
                0 ||
            !a20_object_id_equal(&pending_root, &result.root) ||
            a20_app_stop(&session, &ticket, 9, 4000, &result) < 0 ||
            exited(&session, &ticket, 137, &result) ||
            a20_app_release(&session, &ticket, 4000, &result) < 0)
            return -1;
    }
    if (lose_reply) {
        if (a20_app_offer(&session, short_app, 4000, &offer) < 0 ||
            !offer.token ||
            a20_app_start_offered(&session, short_app, 1200, offer.token, 4000,
                                  &ticket, &result) < 0 ||
            exited(&session, &ticket, 37, &result))
            return -1;
        note("APPS: release-drop-ready\n");
        if (a20_app_release(&session, &ticket, 1500, &result) !=
                -A20_ERR_TIMED_OUT ||
            a20_app_resume(&ticket, 8000, &session) < 0 ||
            a20_app_query(&session, &ticket, 4000, &result) !=
                -A20_ERR_EXPIRED ||
            a20_app_resume(&ticket, 8000, &session) < 0 ||
            a20_app_call(&session, &ticket, A20_APP_START, 0, 4000, &result) !=
                -A20_ERR_EXPIRED ||
            a20_app_resume(&held, 8000, &session) < 0)
            return -1;
        note("APPS: release-timeout-pass old-start=expired\n");
    }
    if (a20_app_query(&session, &held, 4000, &result) < 0 ||
        result.state != A20_APP_RUNNING ||
        !a20_object_id_equal(&root, &result.root) ||
        a20_app_stop(&session, &held, 9, 4000, &result) < 0 ||
        exited(&session, &held, 137, &result) ||
        a20_app_release(&session, &held, 4000, &result) < 0 || result.active ||
        result.occupied)
        return -1;
    a20_app_close(&session);
    note("APPS: recycle-pass real-exits=16 live-preserved old-start=expired "
         "slots=8\n");
    return 0;
}
static int lifecycle(const uint8_t domain[16], uint64_t nonce) {
    a20_app_session_t s;
    a20_app_ticket_t ticket, duplicate;
    a20_app_result_t result;
    if (a20_app_connect(domain, 8000, &s) < 0)
        return -1;
    if (a20_app_start(&s, loop_uuid, nonce, 4000, &ticket, &result) < 0 ||
        result.state != A20_APP_RUNNING)
        return -1;
    a20_object_id_t root = result.root;
    a20_app_ticket_t raced = ticket;
    raced.nonce = nonce + 1000;
    if (a20_app_call(&s, &raced, A20_APP_START, 1, 4000, &result) !=
            -A20_ERR_WOULD_BLOCK ||
        a20_app_resume(&ticket, 8000, &s) < 0)
        return -1;
    if (a20_app_start(&s, loop_uuid, nonce, 4000, &duplicate, &result) < 0 ||
        !a20_object_id_equal(&root, &result.root))
        return -1;
    /* A conflicting payload closes this channel; the exact ticket reconnects. */
    if (a20_app_start(&s, exit_uuid, nonce, 4000, &duplicate, &result) !=
            -A20_ERR_EXISTS ||
        a20_app_resume(&ticket, 8000, &s) < 0)
        return -1;
    if (a20_app_stop(&s, &ticket, 9, 4000, &result) < 0 ||
        exited(&s, &ticket, -1, &result))
        return -1;
    if (a20_app_start(&s, loop_uuid, nonce, 4000, &duplicate, &result) < 0 ||
        result.state != A20_APP_EXITED ||
        !a20_object_id_equal(&root, &result.root))
        return -1;
    if (a20_app_start(&s, exit_uuid, nonce + 1, 4000, &ticket, &result) < 0 ||
        exited(&s, &ticket, 37, &result))
        return -1;
    if (a20_app_start(&s, bad_uuid, nonce + 2, 4000, &ticket, &result) < 0 ||
        result.state != A20_APP_FAILED || result.exit_code >= 0)
        return -1;
    if (a20_app_start(&s, bad_uuid, nonce + 2, 4000, &duplicate, &result) < 0 ||
        result.state != A20_APP_FAILED)
        return -1;
    if (a20_app_start(&s, absent_uuid, nonce + 3, 4000, &duplicate, &result) !=
        -A20_ERR_NOT_SUPPORTED)
        return -1;
    a20_app_close(&s);
    return 0;
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

    a20_memcpy(local_domain, self.bytes, 16);
    if (start_daemon())
        return fail("start daemon");
    if (role) {
        a20_channel_pair_t pair;
        if (a20_channel_create(&pair) < 0 ||
            a20_cluster_export(pair.endpoints[0], "apps.test", 0, 0) < 0)
            return fail("control export");
        a20_hdl_close(pair.endpoints[1]);
        a20_cluster_agent_server_t server;
        a20_work_agent_t unused = {0};
        a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
        server.dispatch = control;
        note("APPS: peer-ready\n");
        for (;;) {
            if (a20_cluster_agent_pump(&server) < 0)
                return fail("control pump");
            sleep_ms(1);
        }
    }
    uint8_t b[16], c[16];
    a20_node_id_t node;
    a20_node_id_parse_hex(identities[1], &node);
    a20_memcpy(b, node.bytes, 16);
    a20_node_id_parse_hex(identities[2], &node);
    a20_memcpy(c, node.bytes, 16);
    if (peer(1, 1) || peer(2, 1))
        return fail("peer ready");
    if (lifecycle(local_domain, 1) || lifecycle(b, 1) || lifecycle(c, 1))
        return fail("mixed lifecycle");
    note("APPS: lifecycle-pass\n");
    uint8_t candidates[3][16];
    a20_memcpy(candidates[0], local_domain, 16);
    a20_memcpy(candidates[1], b, 16);
    a20_memcpy(candidates[2], c, 16);
    a20_app_ticket_t balanced[6];
    unsigned placed[3] = {0};
    a20_app_result_t placed_result;
    for (unsigned i = 0; i < 6; i++) {
        if (a20_app_start_any(candidates, 3, loop_uuid, 20 + i, 30000,
                              &balanced[i], &placed_result) < 0 ||
            placed_result.state != A20_APP_RUNNING)
            return fail("balanced launch");
        int found = 0;
        for (unsigned j = 0; j < 3; j++)
            if (a20_runtime_id_equal(placed_result.root.domain_id,
                                     candidates[j])) {
                placed[j]++;
                found = 1;
            }
        if (!found)
            return fail("placement domain");
    }
    if (placed[0] != 2 || placed[1] != 2 || placed[2] != 2)
        return fail("placement distribution");
    for (unsigned i = 0; i < 3; i++) {
        a20_app_session_t verify;
        if (a20_app_connect(candidates[i], 8000, &verify) < 0 ||
            a20_app_offer(&verify, loop_uuid, 4000, &placed_result) < 0 ||
            placed_result.active != 2)
            return fail("actual active load");
        a20_app_close(&verify);
    }
    /* Let every scheduled child execute its checked workload before stopping.
     * The host separately requires real APP-WORK output on all three ISAs. */
    sleep_ms(3000);
    note("APPS: balance-pass real-programs=6 active=2,2,2\n");
    for (unsigned i = 0; i < 6; i++) {
        a20_app_session_t running;
        if (a20_app_resume(&balanced[i], 8000, &running) < 0 ||
            a20_app_stop(&running, &balanced[i], 9, 4000, &placed_result) < 0 ||
            exited(&running, &balanced[i], -1, &placed_result))
            return fail("balanced stop");
        a20_app_close(&running);
    }

    a20_app_session_t s;
    a20_app_ticket_t ticket, duplicate;
    a20_app_result_t result;
    if (a20_app_connect(b, 8000, &s) < 0)
        return fail("timeout connect");
    note("APPS: drop-ready\n");
    if (a20_app_start(&s, loop_uuid, 99, 1500, &ticket, &result) !=
            -A20_ERR_TIMED_OUT ||
        s.endpoint != A20_HANDLE_NULL)
        return fail("lost start reply");
    if (a20_app_resume(&ticket, 8000, &s) < 0 ||
        a20_app_query(&s, &ticket, 4000, &result) < 0 ||
        result.state != A20_APP_RUNNING)
        return fail("unknown outcome query");
    a20_object_id_t root = result.root;
    note("APPS: timeout-pass\n");
    sleep_ms(100);
    if (a20_app_start(&s, loop_uuid, 99, 4000, &duplicate, &result) < 0 ||
        !a20_object_id_equal(&root, &result.root))
        return fail("explicit retry dedup");
    if (peer(1, 2))
        return fail("daemon restart");
    if (a20_app_query(&s, &ticket, 4000, &result) >= 0 ||
        a20_app_resume(&ticket, 8000, &s) != -A20_ERR_EXPIRED)
        return fail("old controller");
    if (a20_app_connect(b, 8000, &s) < 0 ||
        a20_app_start(&s, loop_uuid, 101, 4000, &ticket, &result) < 0)
        return fail("new controller");
    note("APPS: restart-pass\nAPPS: reboot-ready\n");
    sleep_ms(11000);
    a20_app_close(&s);
    if (peer(1, 1) || a20_app_resume(&ticket, 8000, &s) != -A20_ERR_EXPIRED ||
        lifecycle(b, 201))
        return fail("kernel reboot");
    if (start_configured_daemon() || peer(1, 3) || peer(2, 3))
        return fail("manifest deployment");
    for (unsigned i = 0; i < 3; i++)
        if (cli_run(i))
            return fail("CLI run args/env/exit");
    note("APPS: cli-pass real-args-env exit=37 nodes=3\n");
    if (cli_cancel())
        return fail("CLI signal stop actual remote exit");
    note("APPS: cli-cancel-pass remote-exit=137 active=0\n");
    for (unsigned i = 0; i < 3; i++)
        if (reusable(candidates[i], i == 1))
            return fail("reusable application slots");
    for (unsigned i = 0; i < 9; i++)
        if (cli_run(0))
            return fail("CLI persistent reuse");
    note("APPS: cli-reuse-pass runs=9 no-daemon-restart\n");
    if (linux_applications(candidates))
        return fail("Linux executable lifecycle");
    stop(daemon);
    note("APPS: PASS\n");
    return 0;
}
