/* C Native client for the opt-in Rust IDL service. */
#include "liba20rt/a20_sdk.h"
#include "../svc/a20_services_idl.h"
#include "liba20rt/crt0_a20.h"

#define RUST_ECHOD_PATH "/bin/rust-idl-echod-rv"
#define WAIT_TIMEOUT_NS 2000000000ull
#define TEST_SIGKILL 9u

static a20_handle_t g_out = A20_HANDLE_NULL;
static a20_handle_t g_task = A20_HANDLE_NULL;
static a20_handle_t g_client_ep = A20_HANDLE_NULL;
static a20_handle_t g_ping_ep = A20_HANDLE_NULL;
static int g_task_reaped;

static void say(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    if (g_out != A20_HANDLE_NULL)
        (void)a20_hdl_write_buf(g_out, s, n, 0);
}

static void fail_msg(const char *why)
{
    say("NATIVE_RUST_IDL_SERVICE: FAIL ");
    say(why);
    say("\n");
}

static uint64_t deadline_after(uint64_t ns)
{
    uint64_t now = 0;
    if (a20_clock_get(A20_CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return now + ns;
}

static int pause_before(uint64_t deadline)
{
    uint64_t now = 0;
    if (!deadline || a20_clock_get(A20_CLOCK_MONOTONIC, &now) < 0 || now >= deadline)
        return 0;
    (void)a20_thread_sleep((a20_time_t){ .nsecs = 1000000 });
    return 1;
}

static a20_status_t send_frame(a20_handle_t ep, const void *bytes, uint32_t len)
{
    uint64_t deadline = deadline_after(WAIT_TIMEOUT_NS);
    for (;;) {
        a20_status_t st = a20_channel_send_flags(ep, bytes, len, 0, 0,
                                                  A20_MSG_NONBLOCK);
        if (st >= 0) return st;
        if (st != -A20_ERR_WOULD_BLOCK || !pause_before(deadline)) return st;
    }
}

static a20_status_t receive_frame(a20_handle_t ep, uint8_t *buf, uint32_t cap,
                                  uint32_t *out_len)
{
    uint64_t deadline = deadline_after(WAIT_TIMEOUT_NS);
    for (;;) {
        uint32_t len = cap;
        uint32_t handles = 0;
        a20_status_t st = a20_channel_recv_flags(ep, buf, &len, 0, &handles,
                                                  A20_MSG_NONBLOCK);
        if (st >= 0) {
            if ((uint64_t)st != len || handles != 0) return -A20_ERR_PROTOCOL;
            *out_len = len;
            return A20_OK;
        }
        if (st != -A20_ERR_WOULD_BLOCK || !pause_before(deadline)) return st;
    }
}

static int wait_for_exit(a20_handle_t task, int32_t *exit_code)
{
    uint64_t deadline = deadline_after(WAIT_TIMEOUT_NS);
    for (;;) {
        a20_task_status_t status = { .size = sizeof(status), .version = 1 };
        a20_status_t st = a20_task_wait(task, A20_TASK_WAIT_NONBLOCK, &status);
        if (st >= 0) {
            *exit_code = status.exit_code;
            return 1;
        }
        if (st != -A20_ERR_WOULD_BLOCK || !pause_before(deadline)) return 0;
    }
}

static int cleanup(void)
{
    int clean = 1;
    if (g_task != A20_HANDLE_NULL && !g_task_reaped) {
        (void)a20_task_kill(g_task, TEST_SIGKILL);
        int32_t ignored = 0;
        g_task_reaped = wait_for_exit(g_task, &ignored);
        if (!g_task_reaped) clean = 0;
    }
    if (g_client_ep != A20_HANDLE_NULL) {
        if (a20_hdl_close(g_client_ep) < 0) clean = 0;
        g_client_ep = A20_HANDLE_NULL;
    }
    if (g_ping_ep != A20_HANDLE_NULL) {
        if (a20_hdl_close(g_ping_ep) < 0) clean = 0;
        g_ping_ep = A20_HANDLE_NULL;
    }
    if (g_task != A20_HANDLE_NULL) {
        if (a20_hdl_close(g_task) < 0) clean = 0;
        g_task = A20_HANDLE_NULL;
    }
    return clean;
}

static int fail(const char *why)
{
    fail_msg(why);
    cleanup();
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    a20_channel_pair_t pair;
    a20_channel_pair_t ping;
    if (a20_channel_create(&pair) != A20_OK)
        return fail("channel create");
    if (a20_channel_create(&ping) != A20_OK) {
        (void)a20_hdl_close(pair.endpoints[0]);
        (void)a20_hdl_close(pair.endpoints[1]);
        return fail("ping channel create");
    }

    a20_path_open_args_t open = {
        .dir = A20_HANDLE_NULL,
        .rights = A20_RIGHT_READ | A20_RIGHT_EXEC,
        .path = (uint64_t)(uintptr_t)RUST_ECHOD_PATH,
        .path_len = sizeof(RUST_ECHOD_PATH) - 1,
    };
    if (a20_path_open(&open) != A20_OK || open.out_handle == A20_HANDLE_NULL) {
        (void)a20_hdl_close(pair.endpoints[0]);
        (void)a20_hdl_close(pair.endpoints[1]);
        (void)a20_hdl_close(ping.endpoints[0]);
        (void)a20_hdl_close(ping.endpoints[1]);
        return fail("open Rust service");
    }

    a20_spawn_handle_t child_eps[2] = {
        { .handle = pair.endpoints[1],
          .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
          .target_slot = A20_SVC_ENDPOINT_SLOT },
        { .handle = ping.endpoints[1],
          .rights = A20_RIGHT_READ | A20_RIGHT_WRITE,
          .target_slot = A20_SVC_PING_SLOT },
    };
    a20_task_spawn_args_t spawn = {
        .version = 2,
        .image = open.out_handle,
        .root_dir = si->root_dir,
        .cwd_dir = A20_HANDLE_NULL,
        .event_queue = A20_HANDLE_NULL,
        .handles = (uint64_t)(uintptr_t)child_eps,
        .handle_count = 2,
        .stdin_handle = A20_HANDLE_NULL,
        .stdout_handle = g_out,
        .stderr_handle = g_out,
    };
    a20_status_t st = a20_task_spawn(&spawn);
    (void)a20_hdl_close(open.out_handle);
    (void)a20_hdl_close(pair.endpoints[1]);
    (void)a20_hdl_close(ping.endpoints[1]);
    if (st < 0 || spawn.out_task == A20_HANDLE_NULL) {
        (void)a20_hdl_close(pair.endpoints[0]);
        (void)a20_hdl_close(ping.endpoints[0]);
        return fail("spawn Rust service");
    }
    g_task = spawn.out_task;
    g_client_ep = pair.endpoints[0];
    g_ping_ep = ping.endpoints[0];

    uint8_t request[A20_IDL_ENVELOPE_WIRE_SIZE + A20_IDL_SVCMGR_VERSION_REQUEST_WIRE_SIZE];
    a20_idl_envelope_t env = {
        .version = A20_SERVICES_IDL_VERSION,
        .type = SVCMGR_REQ_VERSION_NEGOTIATE,
        .size = sizeof(request),
    };
    a20_idl_svcmgr_version_request_t body = {
        .min_version = 1, .max_version = 1, .nonce = 0x6b81a9d3u,
    };
    a20_idl_envelope_encode(request, &env);
    a20_idl_svcmgr_version_request_encode(request + A20_IDL_ENVELOPE_WIRE_SIZE,
                                          &body);
    if (send_frame(pair.endpoints[0], request, sizeof(request)) < 0)
        return fail("send negotiation request");
    uint8_t reply[64];
    uint32_t reply_len = 0;
    if (receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != A20_IDL_ENVELOPE_WIRE_SIZE +
                     A20_IDL_SVCMGR_VERSION_RESPONSE_WIRE_SIZE)
        return fail("receive negotiation response");
    a20_idl_envelope_t reply_env;
    a20_idl_envelope_decode(&reply_env, reply);
    a20_idl_svcmgr_version_response_t response;
    a20_idl_svcmgr_version_response_decode(
        &response, reply + A20_IDL_ENVELOPE_WIRE_SIZE);
    if (reply_env.version != A20_SERVICES_IDL_VERSION ||
        reply_env.type != SVCMGR_REP_VERSION_NEGOTIATE ||
        reply_env.size != reply_len || response.min_version != 1 ||
        response.max_version != 1 || response.selected_version != 1 ||
        response.status != SVCMGR_VERSION_NEGOTIATE_OK ||
        response.nonce != body.nonce)
        return fail("bad Rust negotiation response");

    /* Exercise both negative negotiation outcomes against the real Rust
     * server, with a fresh nonce on each request. */
    body.min_version = 2;
    body.max_version = 3;
    body.nonce++;
    a20_idl_svcmgr_version_request_encode(request + A20_IDL_ENVELOPE_WIRE_SIZE,
                                          &body);
    if (send_frame(pair.endpoints[0], request, sizeof(request)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != A20_IDL_ENVELOPE_WIRE_SIZE +
                     A20_IDL_SVCMGR_VERSION_RESPONSE_WIRE_SIZE)
        return fail("no-common negotiation exchange");
    a20_idl_envelope_decode(&reply_env, reply);
    a20_idl_svcmgr_version_response_decode(
        &response, reply + A20_IDL_ENVELOPE_WIRE_SIZE);
    if (reply_env.version != A20_SERVICES_IDL_VERSION ||
        reply_env.type != SVCMGR_REP_VERSION_NEGOTIATE ||
        reply_env.size != reply_len || response.min_version != 1 ||
        response.max_version != 1 ||
        response.status != SVCMGR_VERSION_NEGOTIATE_NO_COMMON ||
        response.selected_version != 0 || response.nonce != body.nonce)
        return fail("bad no-common response");

    body.min_version = 0;
    body.max_version = 1;
    body.nonce++;
    a20_idl_svcmgr_version_request_encode(request + A20_IDL_ENVELOPE_WIRE_SIZE,
                                          &body);
    if (send_frame(pair.endpoints[0], request, sizeof(request)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != A20_IDL_ENVELOPE_WIRE_SIZE +
                     A20_IDL_SVCMGR_VERSION_RESPONSE_WIRE_SIZE)
        return fail("invalid-range negotiation exchange");
    a20_idl_envelope_decode(&reply_env, reply);
    a20_idl_svcmgr_version_response_decode(
        &response, reply + A20_IDL_ENVELOPE_WIRE_SIZE);
    if (reply_env.version != A20_SERVICES_IDL_VERSION ||
        reply_env.type != SVCMGR_REP_VERSION_NEGOTIATE ||
        reply_env.size != reply_len || response.min_version != 1 ||
        response.max_version != 1 ||
        response.status != SVCMGR_VERSION_NEGOTIATE_INVALID_RANGE ||
        response.selected_version != 0 || response.nonce != body.nonce)
        return fail("bad invalid-range response");

    uint8_t echo[A20_IDL_ENVELOPE_WIRE_SIZE + A20_IDL_SVCMGR_ECHO_WIRE_SIZE];
    env.type = SVCMGR_REQ_ECHO;
    env.size = sizeof(echo);
    a20_idl_svcmgr_echo_t echo_body = { .seq = 0x13579bdfu };
    a20_idl_envelope_encode(echo, &env);
    a20_idl_svcmgr_echo_encode(echo + A20_IDL_ENVELOPE_WIRE_SIZE, &echo_body);

    /* A valid request followed by a short malformed frame must not let stale
     * bytes from the previous buffer contents turn that short frame into a
     * second request. The following echo must still receive its own reply. */
    if (send_frame(pair.endpoints[0], echo, sizeof(echo)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != sizeof(echo) || a20_memcmp(reply, echo, sizeof(echo)) != 0)
        return fail("Rust main endpoint echo mismatch");
    uint8_t priming[A20_IDL_ENVELOPE_WIRE_SIZE + A20_IDL_SVCMGR_ECHO_WIRE_SIZE] = {0};
    a20_idl_envelope_t malformed_env = {
        .version = A20_SERVICES_IDL_VERSION,
        .type = SVCMGR_REQ_CRASH,
        .size = 3,
    };
    a20_idl_envelope_encode(priming, &malformed_env);
    if (send_frame(pair.endpoints[0], priming, sizeof(priming)) < 0 ||
        send_frame(pair.endpoints[0], priming, 3) < 0)
        return fail("send short-frame stale-buffer probe");
    echo_body.seq++;
    a20_idl_envelope_encode(echo, &(a20_idl_envelope_t){
        .version = A20_SERVICES_IDL_VERSION,
        .type = SVCMGR_REQ_ECHO,
        .size = sizeof(echo),
    });
    a20_idl_svcmgr_echo_encode(echo + A20_IDL_ENVELOPE_WIRE_SIZE, &echo_body);
    if (send_frame(pair.endpoints[0], echo, sizeof(echo)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != sizeof(echo) || a20_memcmp(reply, echo, sizeof(echo)) != 0)
        return fail("short frame contaminated next request");

    /* The C echod also serves the supervision ping endpoint; verify the Rust
     * replacement echoes there independently of the service channel. */
    echo_body.seq++;
    a20_idl_svcmgr_echo_encode(echo + A20_IDL_ENVELOPE_WIRE_SIZE, &echo_body);
    if (send_frame(ping.endpoints[0], echo, sizeof(echo)) < 0 ||
        receive_frame(ping.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != sizeof(echo) || a20_memcmp(reply, echo, sizeof(echo)) != 0)
        return fail("Rust ping endpoint echo mismatch");

    /* Match C echod's optional-health-channel policy: a CRASH request on ping
     * is consumed without terminating the service, and closing that channel
     * must not prevent later service-endpoint traffic. */
    env.type = SVCMGR_REQ_CRASH;
    env.size = A20_IDL_ENVELOPE_WIRE_SIZE;
    uint8_t crash[A20_IDL_ENVELOPE_WIRE_SIZE];
    a20_idl_envelope_encode(crash, &env);
    if (send_frame(ping.endpoints[0], crash, sizeof(crash)) < 0)
        return fail("send ping crash probe");
    echo_body.seq++;
    a20_idl_envelope_encode(echo, &(a20_idl_envelope_t){
        .version = A20_SERVICES_IDL_VERSION,
        .type = SVCMGR_REQ_ECHO,
        .size = sizeof(echo),
    });
    a20_idl_svcmgr_echo_encode(echo + A20_IDL_ENVELOPE_WIRE_SIZE, &echo_body);
    if (send_frame(pair.endpoints[0], echo, sizeof(echo)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != sizeof(echo) || a20_memcmp(reply, echo, sizeof(echo)) != 0)
        return fail("ping crash terminated service");
    (void)a20_hdl_close(ping.endpoints[0]);
    g_ping_ep = A20_HANDLE_NULL;
    echo_body.seq++;
    a20_idl_svcmgr_echo_encode(echo + A20_IDL_ENVELOPE_WIRE_SIZE, &echo_body);
    if (send_frame(pair.endpoints[0], echo, sizeof(echo)) < 0 ||
        receive_frame(pair.endpoints[0], reply, sizeof(reply), &reply_len) != A20_OK ||
        reply_len != sizeof(echo) || a20_memcmp(reply, echo, sizeof(echo)) != 0)
        return fail("closed ping endpoint stopped service");

    /* Explicit crash remains the established service lifecycle contract. */
    env.type = SVCMGR_REQ_CRASH;
    env.size = A20_IDL_ENVELOPE_WIRE_SIZE;
    a20_idl_envelope_encode(crash, &env);
    if (send_frame(pair.endpoints[0], crash, sizeof(crash)) < 0)
        return fail("send crash request");
    int32_t service_exit = 0;
    if (!wait_for_exit(spawn.out_task, &service_exit))
        return fail("Rust service did not exit with crash code");
    g_task_reaped = 1;
    if (service_exit != A20_SVC_CRASH_CODE)
        return fail("Rust service exited with wrong status");

    if (!cleanup())
        return fail("handle cleanup failed");
    say("NATIVE_RUST_IDL_SERVICE: PASS negotiation=1 echo=1 ping=1 ping-crash=ignored ping-close=alive exit=42\n");
    return 0;
}
