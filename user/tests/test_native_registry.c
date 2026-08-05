/*
 * Registry client e2e (docs/hybrid-kernel/02-mainstream-plan.md M3).
 *
 * Runs as an arbitrary process (not a child of svcmgr): resolves the
 * rtcd service through the well-known registry endpoint from start_info,
 * performs an RPC, crashes the service, re-resolves after the supervisor
 * respawns it, and verifies the RPC works again (automatic rebinding).
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/rtcd_proto.h"

static a20_handle_t g_out = A20_HANDLE_NULL;

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

static void put_i64(int64_t v)
{
    if (v < 0) { put("-", 1); put_u64((uint64_t)(-v)); }
    else put_u64((uint64_t)v);
}

static int fail(int code, const char *msg)
{
    put_str("NATIVE_REGISTRY: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

static a20_status_t reg_lookup(a20_handle_t reg, const char *name,
                               a20_handle_t *out_ep)
{
    uint8_t req[36];
    uint32_t n = 0;
    req[0] = A20_REG_OP_LOOKUP; req[1] = 0; req[2] = 0; req[3] = 0;
    while (name[n] && n < 31) { req[4 + n] = (uint8_t)name[n]; n++; }
    uint32_t req_len = 4 + n;

    int64_t status = -1;
    uint32_t rep_len = sizeof(status);
    a20_handle_t hbuf[1];
    uint32_t hcnt = 1;
    a20_status_t st = a20_channel_call(reg, req, req_len, 0, 0,
                                       &status, &rep_len, hbuf, &hcnt);
    if (st < 0)
        return st;
    if (rep_len != sizeof(status) || status < 0 || hcnt != 1)
        return -1;
    *out_ep = hbuf[0];
    return A20_OK;
}

static int rtcd_time_rpc(a20_handle_t ep, uint64_t *sec_out)
{
    uint8_t req = RTCD_REQ_TIME;
    uint64_t rep[2];
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_h = 0;
    a20_status_t st = a20_channel_call(ep, &req, 1, 0, 0,
                                       rep, &rep_len, 0, &rep_h);
    if (st < 0 || rep_len != sizeof(rep)) {
        put_str("rtcd_time_rpc fail st=");
        put_i64(st);
        put_str(" rep_len=");
        put_i64((int64_t)rep_len);
        put("\n", 1);
        return -1;
    }
    *sec_out = rep[0];
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    a20_handle_t reg = si ? si->service_registry : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;
    if (reg == A20_HANDLE_NULL)
        return fail(1, "no service_registry in start_info");

    /* 1. Resolve rtcd by name and call it. */
    a20_handle_t rtcd = A20_HANDLE_NULL;
    if (reg_lookup(reg, "rtcd", &rtcd) != A20_OK)
        return fail(2, "lookup rtcd failed");
    uint64_t sec = 0;
    if (rtcd_time_rpc(rtcd, &sec) != 0)
        return fail(3, "time RPC failed");
    put_str("NATIVE_REGISTRY: resolved rtc_sec=");
    put_u64(sec);
    put("\n", 1);

    /* 2. Crash the service; the old endpoint dies with it. */
    uint8_t creq = RTCD_REQ_CRASH;
    if (a20_channel_send(rtcd, &creq, 1, 0, 0) != A20_OK)
        return fail(4, "crash request failed");
    a20_hdl_close(rtcd);

    /* 3. Re-resolve: the supervisor respawns and re-registers; retry with
     *    backoff until the fresh endpoint appears.  A successful lookup may
     *    still hand out the just-released old endpoint (the supervisor
     *    updates its table asynchronously), so a CANCELED RPC re-resolves
     *    and retries — this is the documented rebind protocol. */
    a20_handle_t rtcd2 = A20_HANDLE_NULL;
    uint64_t sec2 = 0;
    int rebound = 0;
    for (int i = 0; i < 100 && !rebound; i++) {
        if (reg_lookup(reg, "rtcd", &rtcd2) != A20_OK) {
            a20_time_t bo = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
            a20_thread_sleep(bo);
            continue;
        }
        if (rtcd_time_rpc(rtcd2, &sec2) == 0 && sec2 != 0) {
            rebound = 1;
            break;
        }
        a20_hdl_close(rtcd2);
        rtcd2 = A20_HANDLE_NULL;
        a20_time_t bo = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
        a20_thread_sleep(bo);
    }
    if (!rebound)
        return fail(6, "post-rebind RPC failed");
    put_str("NATIVE_REGISTRY: rebound rtc_sec=");
    put_u64(sec2);
    put("\n", 1);

    a20_hdl_close(rtcd2);
    put_str("NATIVE_REGISTRY: PASS\n");
    return 0;
}
