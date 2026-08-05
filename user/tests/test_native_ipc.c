/*
 * Native ABI IPC fast-path test: fused channel_call RPC.
 *
 * Covers (docs/hybrid-kernel/01-roadmap.md phase 1):
 *   1. functional RPC: client channel_call <-> server recv+send echo
 *   2. handle transfer in both directions over a channel_call exchange
 *   3. NONBLOCK reply stage returns WOULDBLOCK when no reply is queued
 *   4. calling an endpoint whose peer is closed returns CANCELED
 *   5. latency benchmark: send+recv ping-pong vs fused channel_call
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

#define MSG_LEN   32
#define BENCH_ITERS 2000

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

static int fail(int code, const char *msg)
{
    put_str("NATIVE_IPC: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

/* ---- echo server: recv request (+optional handle), send it back ---- */

typedef struct {
    a20_handle_t ep;
    uint32_t     iters;
    uint32_t     fail;
} echo_ctx_t;

static void echo_server_entry(uint64_t argp)
{
    echo_ctx_t *ctx = (echo_ctx_t *)(uintptr_t)argp;
    for (uint32_t i = 0; i < ctx->iters; i++) {
        uint8_t buf[MSG_LEN];
        uint32_t blen = MSG_LEN;
        a20_handle_t hbuf[1];
        uint32_t hcnt = 1;
        a20_status_t st = a20_channel_recv(ctx->ep, buf, &blen, hbuf, &hcnt);
        if (st < 0) { ctx->fail = 1; a20_thread_exit(1); }
        /* Kernel rejects a non-NULL handles pointer when count is 0. */
        st = a20_channel_send(ctx->ep, buf, blen, hcnt ? hbuf : 0, hcnt);
        if (st < 0) { ctx->fail = 2; a20_thread_exit(1); }
        /* The reply transferred our reference away; nothing to close. */
    }
    a20_thread_exit(0);
}

static a20_handle_t start_echo_server(echo_ctx_t *ctx, a20_handle_t server_ep,
                                      uint32_t iters, uint64_t *stack_out)
{
    ctx->ep = server_ep;
    ctx->iters = iters;
    ctx->fail = 0;

    uint64_t stack;
    if (a20_vm_alloc_pages(16, 3 /* RW */, &stack) != A20_OK)
        return A20_HANDLE_NULL;
    *stack_out = stack;

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)echo_server_entry;
    tc.arg = (uint64_t)(uintptr_t)ctx;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL;
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    if (a20_thread_create(&tc) < 0)
        return A20_HANDLE_NULL;
    return 1; /* thread handle not needed for the test protocol */
}

static uint64_t now_ns(void)
{
    a20_time_ns_t t = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &t);
    return t;
}

/* Helper for the decomposition benchmark: yield until told to stop. */
void ipc_yield_worker(uint64_t arg)
{
    volatile uint32_t *go = (volatile uint32_t *)(uintptr_t)arg;
    while (__atomic_load_n(go, __ATOMIC_ACQUIRE))
        a20_thread_yield();
    a20_thread_exit(0);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    /* 1. Functional RPC round trip. */
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK)
        return fail(1, "channel_create failed");

    echo_ctx_t ctx;
    uint64_t stack;
    if (start_echo_server(&ctx, pair.endpoints[1], 1, &stack) == A20_HANDLE_NULL)
        return fail(2, "server start failed");

    uint8_t req[MSG_LEN];
    for (uint32_t i = 0; i < MSG_LEN; i++) req[i] = (uint8_t)(0xA0 + i);
    uint8_t rep[MSG_LEN];
    uint32_t rep_len = MSG_LEN;
    uint32_t rep_hcnt = 0;
    a20_status_t st = a20_channel_call(pair.endpoints[0], req, MSG_LEN,
                                       0, 0, rep, &rep_len, 0, &rep_hcnt);
    if (st < 0)
        return fail(3, "channel_call failed");
    if (rep_len != MSG_LEN)
        return fail(4, "reply length mismatch");
    for (uint32_t i = 0; i < MSG_LEN; i++)
        if (rep[i] != req[i])
            return fail(5, "reply payload mismatch");
    if (ctx.fail)
        return fail(6, "server reported failure");

    /* 2. Handle transfer both ways: send an event queue handle with the
     *    request; the echo server hands it back in the reply. */
    a20_handle_t evq;
    if (a20_event_queue_create(&evq) != A20_OK)
        return fail(7, "event_queue_create failed");

    if (start_echo_server(&ctx, pair.endpoints[1], 1, &stack) == A20_HANDLE_NULL)
        return fail(8, "server restart failed");

    rep_len = MSG_LEN;
    a20_handle_t rep_h[1];
    uint32_t rep_hn = 1;
    st = a20_channel_call(pair.endpoints[0], req, MSG_LEN,
                          &evq, 1, rep, &rep_len, rep_h, &rep_hn);
    if (st < 0)
        return fail(9, "channel_call with handle failed");
    if (rep_hn != 1 || rep_h[0] == A20_HANDLE_NULL)
        return fail(10, "reply handle missing");
    /* The returned handle must be a usable event queue: waiting with a zero
     * timeout on an empty queue yields WOULDBLOCK. */
    {
        a20_event_t evbuf;
        a20_time_t zero = { .secs = 0, .nsecs = 0 };
        st = a20_event_wait(rep_h[0], zero, &evbuf);
        if (st != -A20_ERR_WOULD_BLOCK)
            return fail(11, "returned handle not usable");
    }
    a20_hdl_close(rep_h[0]);
    a20_hdl_close(evq);

    /* 3. NONBLOCK with no server: request is delivered, reply stage polls
     *    empty and reports WOULDBLOCK. */
    {
        a20_channel_pair_t p2;
        if (a20_channel_create(&p2) != A20_OK)
            return fail(12, "channel_create p2 failed");
        rep_len = MSG_LEN;
        rep_hn = 0;
        st = a20_channel_call_flags(p2.endpoints[0], req, MSG_LEN, 0, 0,
                                    rep, &rep_len, 0, &rep_hn,
                                    A20_MSG_NONBLOCK);
        if (st != -A20_ERR_WOULD_BLOCK)
            return fail(13, "NONBLOCK empty reply not WOULDBLOCK");
        /* The queued request must still be receivable by the peer. */
        uint32_t rl = MSG_LEN, rh = 0;
        st = a20_channel_recv_flags(p2.endpoints[1], rep, &rl, 0, &rh,
                                    A20_MSG_NONBLOCK);
        if (st < 0 || rl != MSG_LEN)
            return fail(14, "queued request lost after NONBLOCK call");
        /* 4. Peer closed: send stage reports CANCELED. */
        a20_hdl_close(p2.endpoints[1]);
        rep_len = MSG_LEN;
        st = a20_channel_call(p2.endpoints[0], req, MSG_LEN, 0, 0,
                              rep, &rep_len, 0, &rep_hn);
        if (st != -A20_ERR_CANCELED)
            return fail(15, "call on closed peer not CANCELED");
        a20_hdl_close(p2.endpoints[0]);
    }

    /* 5. Benchmark: ping-pong with send+recv vs fused channel_call. */
    {
        echo_ctx_t bctx;
        if (start_echo_server(&bctx, pair.endpoints[1],
                              BENCH_ITERS * 2, &stack) == A20_HANDLE_NULL)
            return fail(16, "bench server start failed");

        uint64_t t0 = now_ns();
        for (uint32_t i = 0; i < BENCH_ITERS; i++) {
            uint32_t l = MSG_LEN, h = 0;
            if (a20_channel_send(pair.endpoints[0], req, MSG_LEN, 0, 0) < 0)
                return fail(17, "bench send failed");
            if (a20_channel_recv(pair.endpoints[0], rep, &l, 0, &h) < 0)
                return fail(18, "bench recv failed");
        }
        uint64_t t1 = now_ns();
        for (uint32_t i = 0; i < BENCH_ITERS; i++) {
            uint32_t l = MSG_LEN, h = 0;
            if (a20_channel_call(pair.endpoints[0], req, MSG_LEN, 0, 0,
                                 rep, &l, 0, &h) < 0)
                return fail(19, "bench call failed");
        }
        uint64_t t2 = now_ns();
        if (bctx.fail)
            return fail(20, "bench server failure");

        uint64_t legacy_ns = t1 - t0;
        uint64_t fused_ns = t2 - t1;
        put_str("NATIVE_IPC: bench send+recv_ns_per_rt=");
        put_u64(legacy_ns / BENCH_ITERS);
        put_str(" call_ns_per_rt=");
        put_u64(fused_ns / BENCH_ITERS);
        put_str(" x100_ratio=");
        put_u64(fused_ns * 100 / (legacy_ns ? legacy_ns : 1));
        put("\n", 1);
    }

    /* 6. Cost decomposition for M1 (direct-switch design input):
     *   solo: 2 traps + enqueue/dequeue, 0 context switches
     *   yield: 2 context switches, 0 traps
     *   ping-pong ≈ 4 traps + 2 switches (from bench above)
     */
    {
        a20_channel_pair_t solo;
        if (a20_channel_create(&solo) != A20_OK)
            return fail(21, "solo channel failed");
        uint64_t t0 = now_ns();
        for (uint32_t i = 0; i < BENCH_ITERS; i++) {
            uint32_t l = MSG_LEN, h = 0;
            if (a20_channel_send(solo.endpoints[0], req, MSG_LEN, 0, 0) < 0)
                return fail(22, "solo send failed");
            if (a20_channel_recv(solo.endpoints[1], rep, &l, 0, &h) < 0)
                return fail(23, "solo recv failed");
        }
        uint64_t t1 = now_ns();

        /* yield ping-pong with the bench server thread (it exited after
         * BENCH_ITERS*2; spawn a fresh one that just yields). */
        uint64_t stack2;
        volatile uint32_t go = 1;
        if (a20_vm_alloc_pages(16, 3, &stack2) != A20_OK)
            return fail(24, "yield stack failed");
        extern void ipc_yield_worker(uint64_t arg);
        a20_thread_create_args_t tc = {0};
        tc.entry = (uint64_t)ipc_yield_worker;
        tc.arg = (uint64_t)(uintptr_t)&go;
        tc.stack_base = (stack2 + 16 * 4096) & ~15ULL;
        tc.stack_size = 16 * 4096;
        tc.tls_base = 0;
        if (a20_thread_create(&tc) < 0)
            return fail(25, "yield thread failed");
        uint64_t t2 = now_ns();
        for (uint32_t i = 0; i < BENCH_ITERS; i++)
            a20_thread_yield();
        uint64_t t3 = now_ns();
        go = 0;
        a20_thread_yield();

        uint64_t solo_ns = t1 - t0;
        uint64_t yield_ns = t3 - t2;
        put_str("NATIVE_IPC: decomp solo_2trap_ns=");
        put_u64(solo_ns / BENCH_ITERS);
        put_str(" yield_ns=");
        put_u64(yield_ns / BENCH_ITERS);
        put("\n", 1);
        a20_hdl_close(solo.endpoints[0]);
        a20_hdl_close(solo.endpoints[1]);
    }

    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);

    put_str("NATIVE_IPC: PASS\n");
    return 0;
}
