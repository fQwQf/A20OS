/*
 * Registry client e2e (docs/hybrid-kernel/02-mainstream-plan.md M3).
 *
 * Runs as an arbitrary process (not a child of svcmgr): resolves the
 * rtcd service through the well-known registry endpoint from start_info,
 * performs an RPC, crashes the service, re-resolves after the supervisor
 * respawns it, and verifies the RPC works again (automatic rebinding).
 *
 * It then exercises the supervision force-stop path: SVCMGR_REQ_HANG stops
 * echod's pump, the supervisor's health deadline must force-exit it and
 * restart it, and a client holding only the service endpoint must not be
 * able to force-stop it (permission/identity negative control).
 */
#include "liba20rt/a20_sdk.h"
#include "../svc/a20_services_idl.h"
#include "liba20rt/crt0_a20.h"

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
    /* rtcd parses versioned IDL envelopes (a20_services.idl), so the
     * request must carry the 8-byte header, not the legacy raw 'T'. */
    struct {
        a20_idl_envelope_t env;
    } req = {
        { A20_SERVICES_IDL_VERSION, RTCD_REQ_TIME, sizeof(req) },
    };
    struct {
        a20_idl_envelope_t env;
        a20_idl_rtcd_time_response_t body;
    } rep;
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_h = 0;
    a20_status_t st = a20_channel_call(ep, &req, sizeof(req), 0, 0,
                                       &rep, &rep_len, 0, &rep_h);
    if (st < 0 || rep_len != sizeof(rep) ||
        rep.env.type != RTCD_REPLY_TIME) {
        put_str("rtcd_time_rpc fail st=");
        put_i64(st);
        put_str(" rep_len=");
        put_i64((int64_t)rep_len);
        put("\n", 1);
        return -1;
    }
    *sec_out = rep.body.seconds;
    return 0;
}

static int echod_echo_rpc(a20_handle_t reg)
{
    a20_handle_t ep = A20_HANDLE_NULL;
    if (reg_lookup(reg, "echod", &ep) != A20_OK)
        return -1;

    struct {
        a20_idl_envelope_t env;
        a20_idl_svcmgr_echo_t body;
    } req = {
        { A20_SERVICES_IDL_VERSION, SVCMGR_REQ_ECHO,
          sizeof(a20_idl_envelope_t) + A20_IDL_SVCMGR_ECHO_WIRE_SIZE },
        { .seq = 0x51ec0deu },
    };
    uint8_t req_wire[sizeof(req)];
    a20_idl_envelope_encode(req_wire, &req.env);
    a20_idl_svcmgr_echo_encode(req_wire + A20_IDL_ENVELOPE_WIRE_SIZE,
                               &req.body);

    uint8_t rep[sizeof(req_wire)];
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_handles = 0;
    a20_status_t st = a20_channel_call(ep, req_wire, sizeof(req_wire),
                                       0, 0, rep, &rep_len, 0, &rep_handles);
    a20_hdl_close(ep);
    if (st < 0 || rep_handles != 0 ||
        rep_len != A20_IDL_ENVELOPE_WIRE_SIZE + A20_IDL_SVCMGR_ECHO_WIRE_SIZE)
        return -1;

    a20_idl_envelope_t env;
    a20_idl_envelope_decode(&env, rep);
    a20_idl_svcmgr_echo_t body;
    a20_idl_svcmgr_echo_decode(&body, rep + A20_IDL_ENVELOPE_WIRE_SIZE);
    return env.version == A20_SERVICES_IDL_VERSION &&
           env.type == SVCMGR_REQ_ECHO && env.size == rep_len &&
           body.seq == req.body.seq ? 0 : -1;
}

/* Nonblocking echo probe against a service endpoint: 1 = byte-identical
 * echo received (the service is pumping), <= 0 = silent, revoked, or an
 * unmatched reply.  The byte compare keeps a late reply to an earlier
 * probe from ever reading as "alive" for a later one. */
static int echod_probe(a20_handle_t ep, uint32_t seq)
{
    struct {
        a20_idl_envelope_t env;
        a20_idl_svcmgr_echo_t body;
    } req = {
        { A20_SERVICES_IDL_VERSION, SVCMGR_REQ_ECHO,
          sizeof(a20_idl_envelope_t) + A20_IDL_SVCMGR_ECHO_WIRE_SIZE },
        { .seq = seq },
    };
    uint8_t req_wire[sizeof(req)];
    a20_idl_envelope_encode(req_wire, &req.env);
    a20_idl_svcmgr_echo_encode(req_wire + A20_IDL_ENVELOPE_WIRE_SIZE,
                               &req.body);

    uint8_t rep[sizeof(req_wire)];
    uint32_t rep_len = sizeof(rep);
    uint32_t rep_handles = 0;
    a20_status_t st = a20_channel_call_flags(ep, req_wire, sizeof(req_wire),
                                             0, 0, rep, &rep_len, 0,
                                             &rep_handles, A20_MSG_NONBLOCK);
    /* A live service needs a scheduler round-trip; a hung or dead one
     * never answers, so this window is the liveness oracle. */
    for (int i = 0; st == -A20_ERR_WOULD_BLOCK && i < 25; i++) {
        a20_time_t nap = { .secs = 0, .nsecs = 2 * 1000 * 1000 };
        a20_thread_sleep(nap);
        rep_len = sizeof(rep);
        rep_handles = 0;
        st = a20_channel_recv_flags(ep, rep, &rep_len, 0, &rep_handles,
                                    A20_MSG_NONBLOCK);
    }
    if (st < 0)
        return 0;
    if (rep_len != sizeof(req_wire) || rep_handles != 0)
        return 0;
    for (uint32_t i = 0; i < sizeof(req_wire); i++) {
        if (rep[i] != req_wire[i])
            return 0;
    }
    return 1;
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

    /* Exercise the runtime echod service selected by the manager, not only
     * the standalone Rust IDL fixture. Both C and Rust services echo this
     * established v1 request byte-for-byte. */
    if (echod_echo_rpc(reg) != 0)
        return fail(7, "echod echo RPC failed");
    put_str("NATIVE_REGISTRY: echod echo=PASS\n");

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
    a20_idl_envelope_t creq = {
        A20_SERVICES_IDL_VERSION, RTCD_REQ_CRASH, sizeof(creq)
    };
    if (a20_channel_send(rtcd, &creq, sizeof(creq), 0, 0) != A20_OK)
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

    /* 4. Supervision self-test: HANG echod and verify the force-stop ->
     *    restart -> recovery sequence.  echod stops answering both its
     *    service and health channels; the supervisor's health deadline must
     *    force-exit it (128+9 = 137) and respawn it. */
    a20_handle_t hang_ep = A20_HANDLE_NULL;
    if (reg_lookup(reg, "echod", &hang_ep) != A20_OK)
        return fail(8, "lookup echod for hang failed");
    a20_idl_envelope_t hang = {
        A20_SERVICES_IDL_VERSION, SVCMGR_REQ_HANG, sizeof(hang)
    };
    if (a20_channel_send(hang_ep, &hang, sizeof(hang), 0, 0) != A20_OK)
        return fail(9, "hang request failed");

    /* Confirm the service really stopped pumping: three consecutive silent
     * probes.  A late echo of an earlier probe cannot pass the byte
     * compare, and a live echod answers well inside the probe window. */
    int silent_run = 0;
    for (int i = 0; i < 40 && silent_run < 3; i++) {
        if (echod_probe(hang_ep, 0x600d0000u + (uint32_t)i) == 1)
            silent_run = 0;
        else
            silent_run++;
        a20_time_t nap = { .secs = 0, .nsecs = 10 * 1000 * 1000 };
        a20_thread_sleep(nap);
    }
    if (silent_run < 3)
        return fail(10, "echod kept answering after HANG");
    put_str("NATIVE_REGISTRY: echod hang silent=PASS\n");

    /* Re-resolve until a pumping generation appears.  The hung generation
     * can never answer again, so a valid echo proves the restart. */
    a20_handle_t heal_ep = A20_HANDLE_NULL;
    int heal_gap = 0;
    int healed = 0;
    for (int i = 0; i < 500 && !healed; i++) {
        a20_handle_t ep = A20_HANDLE_NULL;
        if (reg_lookup(reg, "echod", &ep) != A20_OK) {
            heal_gap = 1;
        } else if (echod_probe(ep, 0x600d8000u + (uint32_t)i) == 1) {
            healed = 1;
            heal_ep = ep;
            ep = A20_HANDLE_NULL;
        }
        if (ep != A20_HANDLE_NULL)
            a20_hdl_close(ep);
        if (!healed) {
            a20_time_t nap = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
            a20_thread_sleep(nap);
        }
    }
    if (!healed)
        return fail(11, "echod was not restarted after HANG");
    put_str("NATIVE_REGISTRY: echod healed echo=PASS");
    if (heal_gap)
        put_str(" (registry unbound while down)");
    put("\n", 1);

    /* The stopped generation's endpoint must stay dead: its channel peer is
     * gone for good, so an echo here would mean the same task resumed
     * instead of a fresh generation answering. */
    if (echod_probe(hang_ep, 0x600dff00u) == 1)
        return fail(12, "old echod endpoint answered after restart");
    a20_hdl_close(hang_ep);

    /* 5. Permission/identity negative control: the client holds only a
     *    service endpoint handle and must not be able to force-stop the
     *    service through it. */
    a20_status_t kill_st = a20_task_kill(heal_ep, 9);
    if (kill_st >= 0)
        return fail(13, "foreign force-stop through endpoint accepted");
    put_str("NATIVE_REGISTRY: foreign kill rejected st=");
    put_i64(kill_st);
    put("\n", 1);
    if (echod_probe(heal_ep, 0x600dabcdu) != 1)
        return fail(14, "echod gone after rejected force-stop");
    put_str("NATIVE_REGISTRY: echod alive after rejected kill=PASS\n");
    a20_hdl_close(heal_ep);

    put_str("NATIVE_REGISTRY: PASS\n");
    return 0;
}
