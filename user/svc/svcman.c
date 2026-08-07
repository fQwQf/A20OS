/*
 * svcman — hybrid-kernel service supervisor + self-healing demo driver.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §5.
 *
 * Responsibilities demonstrated end to end:
 *   1. spawn a service (echod) via task_spawn, passing its service channel
 *      endpoint at a fixed handle slot and inheriting stdio;
 *   2. act as an RPC client over the fused channel_call fast path;
 *   3. watch the service task for A20_EVENT_EXITED via an event queue;
 *   4. on crash: report the exit code, back off, respawn with a fresh
 *      channel, and verify the service is functional again.
 *
 * Prints NATIVE_SVC: PASS when the full crash/heal cycle succeeds twice.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/svc_proto.h"

#define ECHOD_PATH   "/bin/svc-echod-rv"
#define BACKOFF_MS   50

static a20_handle_t g_out = A20_HANDLE_NULL;
static a20_handle_t g_root = A20_HANDLE_NULL;

static void put(const char *s, uint32_t len)
{
    a20_hdl_write_buf(g_out, s, len, (void *)0);
}

static void put_str(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    put(s, n);
}

static void put_u64(uint64_t v)
{
    char buf[24];
    int i = 24;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    put(buf + i, (uint32_t)(24 - i));
}

static int fail(int code, const char *msg)
{
    put_str("NATIVE_SVC: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

/*
 * spawn_echod — create a fresh channel pair and spawn echod with the
 * service endpoint installed at A20_SVC_ENDPOINT_SLOT.  On success
 * *out_ep holds the supervisor's client endpoint (READ|WRITE) and
 * *out_task the service task handle; the caller watches the task for
 * A20_EVENT_EXITED.
 */
static a20_status_t spawn_echod(a20_handle_t *out_ep, a20_handle_t *out_task)
{
    a20_channel_pair_t pair;
    a20_status_t st = a20_channel_create(&pair);
    if (st != A20_OK)
        return st;

    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)ECHOD_PATH;
    oa.path_len = sizeof(ECHOD_PATH) - 1;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    st = a20_path_open(&oa);
    if (st != A20_OK) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        return st;
    }

    a20_spawn_handle_t sh;
    sh.handle = pair.endpoints[1];
    sh.rights = A20_RIGHT_READ | A20_RIGHT_WRITE;
    sh.target_slot = A20_SVC_ENDPOINT_SLOT;
    sh.flags = 0;

    /* v2 args: inherit our stdout/stderr so the service can log. */
    a20_task_spawn_args_t ta;
    ta.size = sizeof(ta);
    ta.version = 2;
    ta.image = oa.out_handle;
    ta.root_dir = g_root;
    ta.cwd_dir = A20_HANDLE_NULL;
    ta.event_queue = A20_HANDLE_NULL;
    ta.argv = 0;
    ta.envp = 0;
    ta.argc = 0;
    ta.envc = 0;
    ta.handles = (uint64_t)(uintptr_t)&sh;
    ta.handle_count = 1;
    ta.flags = 0;
    ta.out_task = A20_HANDLE_NULL;
    ta.stdin_handle = A20_HANDLE_NULL;
    ta.stdout_handle = g_out;
    ta.stderr_handle = g_out;
    ta.reserved = 0;

    st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)&ta, 0, 0, 0, 0, 0);
    a20_hdl_close(oa.out_handle);
    a20_hdl_close(pair.endpoints[1]); /* object lives on in the child */
    if (st < 0) {
        a20_hdl_close(pair.endpoints[0]);
        return st;
    }

    *out_ep = pair.endpoints[0];
    *out_task = ta.out_task;
    return A20_OK;
}

/* One RPC round trip; returns A20_OK when the reply echoes the request. */
static a20_status_t echo_verify(a20_handle_t ep, uint32_t seq)
{
    struct {
        a20_idl_envelope_t env;
        a20_idl_svcmgr_echo_t body;
    } req = {
        { A20_SERVICES_IDL_VERSION, SVCMGR_REQ_ECHO, sizeof(req) },
        { seq },
    };
    uint8_t rep[sizeof(req)];
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_hcnt = 0;
    a20_status_t st = a20_channel_call(ep, &req, sizeof(req), 0, 0,
                                       rep, &rep_len, 0, &rep_hcnt);
    if (st < 0)
        return st;
    if (rep_len != sizeof(req))
        return -1000;
    if (a20_memcmp(rep, &req, sizeof(req)) != 0)
        return -1001;
    return A20_OK;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_root = si ? si->root_dir : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK)
        return fail(1, "event_queue_create failed");

    uint32_t restarts = 0;
    a20_handle_t ep = A20_HANDLE_NULL;
    a20_handle_t task = A20_HANDLE_NULL;

    if (spawn_echod(&ep, &task) != A20_OK)
        return fail(2, "initial spawn failed");
    if (a20_event_watch(eq, task, A20_EVENT_MASK(A20_EVENT_EXITED), 0) != A20_OK)
        return fail(3, "event_watch failed");

    /* Service up: first RPC. */
    if (echo_verify(ep, 0) != A20_OK)
        return fail(4, "initial echo failed");
    put_str("NATIVE_SVC: service ready\n");

    for (restarts = 1; restarts <= 2; restarts++) {
        /* Ask the service to crash; no reply is sent for this request. */
        a20_idl_envelope_t crash_req = {
            A20_SERVICES_IDL_VERSION, SVCMGR_REQ_CRASH, sizeof(crash_req)
        };
        if (a20_channel_send(ep, &crash_req, sizeof(crash_req), 0, 0) != A20_OK)
            return fail(5, "crash request failed");

        /* Wait for the EXITED event (bounded: 2 s). */
        a20_event_t ev;
        a20_time_t to = { .secs = 2, .nsecs = 0 };
        a20_status_t st = a20_event_wait(eq, to, &ev);
        if (st < 0)
            return fail(6, "EXITED event not received");
        put_str("NATIVE_SVC: crash detected exit_code=");
        put_u64(ev.data0);
        put("\n", 1);
        if (ev.data0 != A20_SVC_CRASH_CODE)
            return fail(7, "unexpected exit code");

        /* Old endpoint sees PEER_CLOSED now; retire it and the task handle. */
        a20_event_cancel(eq, task);
        a20_hdl_close(task);
        a20_hdl_close(ep);

        /* Backoff, then respawn with a fresh channel. */
        a20_time_t bo = { .secs = 0, .nsecs = BACKOFF_MS * 1000 * 1000 };
        a20_thread_sleep(bo);

        if (spawn_echod(&ep, &task) != A20_OK)
            return fail(8, "respawn failed");
        if (a20_event_watch(eq, task, A20_EVENT_MASK(A20_EVENT_EXITED), 0) != A20_OK)
            return fail(9, "re-watch failed");
        if (echo_verify(ep, restarts) != A20_OK)
            return fail(10, "post-restart echo failed");
        put_str("NATIVE_SVC: healed restart=");
        put_u64(restarts);
        put("\n", 1);
    }

    /* Clean shutdown: kill the service, observe EXITED, release everything. */
    a20_task_kill(task, 0);
    a20_event_cancel(eq, task);
    a20_hdl_close(task);
    a20_hdl_close(ep);
    a20_hdl_close(eq);

    put_str("NATIVE_SVC: PASS restarts=");
    put_u64(2);
    put("\n", 1);
    return 0;
}
