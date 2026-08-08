/*
 * rtcd supervisor e2e test (docs/hybrid-kernel/01-roadmap.md phase 4).
 *
 * Spawns the user-space RTC driver service and verifies end to end:
 *   1. time RPC: goldfish RTC read via user-mapped MMIO (sec plausible);
 *   2. alarm IRQ: arm 100 ms alarm, receive the async reply posted when
 *      the kernel routed IRQ 11 to the driver's event queue;
 *   3. crash self-heal: kill the driver, supervisor respawns it and the
 *      time RPC works again (user-space driver crash != system crash).
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/rtcd_proto.h"

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
    put_str("NATIVE_RTCD: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

static uint64_t now_ns(void)
{
    a20_time_ns_t t = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &t);
    return t;
}

static a20_status_t spawn_rtcd(a20_handle_t *out_ep, a20_handle_t *out_task)
{
    a20_channel_pair_t pair;
    a20_status_t st = a20_channel_create(&pair);
    if (st != A20_OK)
        return st;

    static const char path[] = "/bin/rtcd-rv.a20drv";
    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)path;
    oa.path_len = sizeof(path) - 1;
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
    sh.target_slot = A20_RTCD_EP_SLOT;
    sh.flags = 0;

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
    a20_hdl_close(pair.endpoints[1]);
    if (st < 0) {
        a20_hdl_close(pair.endpoints[0]);
        return st;
    }
    *out_ep = pair.endpoints[0];
    *out_task = ta.out_task;
    return A20_OK;
}

/* 'T' RPC: returns 0 with *sec_out set on success. */
static int rtcd_time_rpc(a20_handle_t ep, uint64_t *sec_out)
{
    a20_idl_envelope_t req = {
        A20_SERVICES_IDL_VERSION, RTCD_REQ_TIME,
        sizeof(a20_idl_envelope_t)
    };
    uint8_t rep[sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_time_response_t)];
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_h = 0;
    a20_status_t st = a20_channel_call(ep, &req, sizeof(req), 0, 0,
                                       rep, &rep_len, 0, &rep_h);
    if (st < 0 || rep_len != sizeof(rep))
        return -1;
    a20_idl_envelope_t env;
    a20_memcpy(&env, rep, sizeof(env));
    if (env.version != A20_SERVICES_IDL_VERSION || env.type != RTCD_REPLY_TIME ||
        env.size != rep_len)
        return -1;
    a20_idl_rtcd_time_response_t body;
    a20_memcpy(&body, rep + sizeof(env), sizeof(body));
    *sec_out = body.seconds;
    return 0;
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

    a20_handle_t ep, task;
    if (spawn_rtcd(&ep, &task) != A20_OK)
        return fail(2, "spawn rtcd failed");
    if (a20_event_watch(eq, task, A20_EVENT_MASK(A20_EVENT_EXITED), 0) != A20_OK)
        return fail(3, "event_watch failed");

    /* 1. Time RPC: user-space MMIO read of the goldfish RTC. */
    uint64_t sec = 0;
    if (rtcd_time_rpc(ep, &sec) != 0)
        return fail(4, "time RPC failed");
    put_str("NATIVE_RTCD: rtc_sec=");
    put_u64(sec);
    put("\n", 1);
    if (sec < 1577836800ULL /* 2020-01-01 */ || sec > 4102444800ULL /* 2100 */)
        return fail(5, "rtc seconds implausible");

    /* 2. Alarm IRQ: async reply must arrive after ~100 ms. */
    uint8_t areq[sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_alarm_request_t)] = {0};
    a20_idl_envelope_t aenv = {
        A20_SERVICES_IDL_VERSION, RTCD_REQ_ALARM, sizeof(areq)
    };
    a20_memcpy(areq, &aenv, sizeof(aenv));
    a20_idl_rtcd_alarm_request_t alarm = { .milliseconds = 100 };
    a20_memcpy(&areq[sizeof(aenv)], &alarm, sizeof(alarm));
    uint64_t t0 = now_ns();
    if (a20_channel_send(ep, areq, sizeof(areq), 0, 0) != A20_OK)
        return fail(6, "alarm request failed");
    uint8_t ans[sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_time_response_t)];
    uint32_t alen = sizeof(ans);
    uint32_t ah = 0;
    if (a20_channel_recv(ep, ans, &alen, 0, &ah) < 0 || alen != sizeof(ans))
        return fail(7, "alarm reply not received");
    {
        a20_idl_envelope_t aenv;
        a20_memcpy(&aenv, ans, sizeof(aenv));
        if (aenv.version != A20_SERVICES_IDL_VERSION ||
            aenv.type != RTCD_REPLY_ALARM || aenv.size != alen)
            return fail(7, "alarm reply envelope invalid");
    }
    uint64_t elapsed = now_ns() - t0;
    put_str("NATIVE_RTCD: alarm_ms=");
    put_u64(elapsed / 1000000);
    put("\n", 1);
    if (elapsed < 50ULL * 1000 * 1000 || elapsed > 5000ULL * 1000 * 1000)
        return fail(8, "alarm timing out of bounds");

    /* 3. Crash self-heal: kill the driver, watch EXITED, respawn, re-RPC. */
    a20_idl_envelope_t creq = {
        A20_SERVICES_IDL_VERSION, RTCD_REQ_CRASH, sizeof(creq)
    };
    if (a20_channel_send(ep, &creq, sizeof(creq), 0, 0) != A20_OK)
        return fail(9, "crash request failed");
    {
        a20_event_t ev;
        a20_time_t to = { .secs = 2, .nsecs = 0 };
        if (a20_event_wait(eq, to, &ev) < 0)
            return fail(10, "EXITED not observed");
        put_str("NATIVE_RTCD: driver crash detected exit_code=");
        put_u64(ev.data0);
        put("\n", 1);
    }
    a20_event_cancel(eq, task);
    a20_hdl_close(task);
    a20_hdl_close(ep);

    a20_time_t bo = { .secs = 0, .nsecs = 50 * 1000 * 1000 };
    a20_thread_sleep(bo);

    if (spawn_rtcd(&ep, &task) != A20_OK)
        return fail(11, "respawn failed");
    if (a20_event_watch(eq, task, A20_EVENT_MASK(A20_EVENT_EXITED), 0) != A20_OK)
        return fail(12, "re-watch failed");
    uint64_t sec2 = 0;
    if (rtcd_time_rpc(ep, &sec2) != 0 || sec2 == 0)
        return fail(13, "post-restart RPC failed");
    put_str("NATIVE_RTCD: healed rtc_sec=");
    put_u64(sec2);
    put("\n", 1);

    a20_task_kill(task, 0);
    a20_event_cancel(eq, task);
    a20_hdl_close(task);
    a20_hdl_close(ep);
    a20_hdl_close(eq);

    put_str("NATIVE_RTCD: PASS\n");
    return 0;
}
