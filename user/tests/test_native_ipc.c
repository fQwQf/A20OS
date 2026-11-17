/*
 * Native ABI IPC fast-path test: fused channel_call RPC.
 *
 * Covers (docs/zh/hybrid-kernel/01-roadmap.md phase 1):
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

static int is_wake_race_arg(const char *arg)
{
    static const char expected[] = "--wake-race";
    if (!arg) return 0;
    for (uint32_t i = 0; i < sizeof(expected); i++)
        if (arg[i] != expected[i]) return 0;
    return 1;
}

static int is_queued_reply_arg(const char *arg)
{
    static const char expected[] = "--queued-reply";
    if (!arg) return 0;
    for (uint32_t i = 0; i < sizeof(expected); i++)
        if (arg[i] != expected[i]) return 0;
    return 1;
}

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
    volatile uint32_t ready;
    volatile uint32_t done;
} echo_ctx_t;

static echo_ctx_t g_donate_test_ctx;
static uint64_t g_donate_test_stack;
static echo_ctx_t g_queued_reply_ctx;
static uint64_t g_queued_reply_stack;

static void echo_server_entry(uint64_t argp)
{
    echo_ctx_t *ctx = (echo_ctx_t *)(uintptr_t)argp;
    for (uint32_t i = 0; i < ctx->iters; i++) {
        uint8_t buf[MSG_LEN];
        uint32_t blen = MSG_LEN;
        a20_handle_t hbuf[1];
        uint32_t hcnt = 1;
        __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
        a20_status_t st = a20_channel_recv(ctx->ep, buf, &blen, hbuf, &hcnt);
        if (st < 0) {
            ctx->fail = 1;
            __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
            a20_thread_exit(1);
        }
        /* Kernel rejects a non-NULL handles pointer when count is 0. */
        st = a20_channel_send(ctx->ep, buf, blen, hcnt ? hbuf : 0, hcnt);
        if (st < 0) {
            ctx->fail = 2;
            __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
            a20_thread_exit(1);
        }
        /* The reply transferred our reference away; nothing to close. */
    }
    __atomic_store_n(&ctx->done, 1, __ATOMIC_RELEASE);
    a20_thread_exit(0);
}

static a20_handle_t start_echo_server(echo_ctx_t *ctx, a20_handle_t server_ep,
                                      uint32_t iters, uint64_t *stack_out)
{
    ctx->ep = server_ep;
    ctx->iters = iters;
    ctx->fail = 0;
    __atomic_store_n(&ctx->ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->done, 0, __ATOMIC_RELEASE);

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

static int test_recv_transaction_rollback(void)
{
    put_str("NATIVE_IPC: recv-txn pre-dequeue start\n");
    a20_channel_pair_t pair;
    a20_handle_t sent[2], received[2] = {A20_HANDLE_NULL, A20_HANDLE_NULL};
    if (a20_channel_create(&pair) != A20_OK ||
        a20_event_queue_create(&sent[0]) != A20_OK ||
        a20_event_queue_create(&sent[1]) != A20_OK)
        return fail(40, "recv rollback setup failed");
    if (a20_channel_send(pair.endpoints[0], 0, 0, sent, 2) < 0)
        return fail(41, "recv rollback send failed");

    /* A too-small user handle array must fail before dequeue; retrying with
     * enough space receives the same queued message. */
    uint32_t length = 0, count = 1;
    a20_status_t st = a20_channel_recv_flags(pair.endpoints[1], 0, &length,
                                             received, &count,
                                             A20_MSG_NONBLOCK);
    if (st != -A20_ERR_NO_SPACE)
        return fail(42, "short handle buffer did not return NO_SPACE");
    count = 2;
    st = a20_channel_recv_flags(pair.endpoints[1], 0, &length, received,
                                &count, A20_MSG_NONBLOCK);
    if (st < 0 || count != 2)
        return fail(43, "pre-dequeue failure consumed queued message");
    a20_hdl_close(received[0]);
    a20_hdl_close(received[1]);
    a20_hdl_close(sent[0]);
    a20_hdl_close(sent[1]);
    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    put_str("NATIVE_IPC: recv-txn pre-dequeue PASS\n");

    /* Force a usercopy fault after the first handle word has crossed into a
     * valid page. The syscall consumes the dequeued message, but rolls back
     * the installed capability so the partially written value is stale. */
    put_str("NATIVE_IPC: recv-txn partial-copy start\n");
    if (a20_channel_create(&pair) != A20_OK ||
        a20_event_queue_create(&sent[0]) != A20_OK ||
        a20_event_queue_create(&sent[1]) != A20_OK)
        return fail(44, "partial-copy setup failed");
    if (a20_channel_send(pair.endpoints[0], 0, 0, sent, 2) < 0)
        return fail(45, "partial-copy send failed");
    uint64_t pages;
    if (a20_vm_alloc_pages(2, A20_PROT_READ | A20_PROT_WRITE, &pages) < 0)
        return fail(46, "partial-copy pages allocation failed");
    if (a20_vm_unmap(pages + 4096, 4096) < 0)
        return fail(47, "partial-copy guard page setup failed");
    a20_handle_t *crossing = (a20_handle_t *)(uintptr_t)(pages + 4096 -
                                                          sizeof(a20_handle_t));
    *crossing = 0xeeeeeeeeu;
    length = 0;
    count = 2;
    st = a20_channel_recv_flags(pair.endpoints[1], 0, &length, crossing,
                                &count, A20_MSG_NONBLOCK);
    if (st != -A20_ERR_FAULT)
        return fail(48, "cross-page output did not fault");
    if (*crossing == 0xeeeeeeeeu || a20_hdl_close(*crossing) != -A20_ERR_BAD_HANDLE)
        return fail(49, "partially copied handle remained installed");
    length = 0;
    count = 0;
    st = a20_channel_recv_flags(pair.endpoints[1], 0, &length, 0, &count,
                                A20_MSG_NONBLOCK);
    if (st != -A20_ERR_WOULD_BLOCK)
        return fail(50, "post-dequeue fault did not consume message");
    a20_vm_unmap(pages, 4096);
    a20_hdl_close(sent[0]);
    a20_hdl_close(sent[1]);
    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    put_str("NATIVE_IPC: recv-txn partial-copy PASS\n");

    /* The fused call's final argument-structure copy is also inside the
     * transaction: make its args readable but not writable after setup. */
    put_str("NATIVE_IPC: recv-txn final-args start\n");
    if (a20_channel_create(&pair) != A20_OK ||
        a20_event_queue_create(&sent[0]) != A20_OK)
        return fail(51, "call publish setup failed");
    put_str("NATIVE_IPC: recv-txn final-args endpoints ready\n");
    echo_ctx_t ctx;
    uint64_t stack;
    if (start_echo_server(&ctx, pair.endpoints[1], 1, &stack) == A20_HANDLE_NULL)
        return fail(52, "call publish server start failed");
    put_str("NATIVE_IPC: recv-txn final-args server ready\n");
    uint64_t args_page;
    if (a20_vm_alloc_pages(1, A20_PROT_READ | A20_PROT_WRITE, &args_page) < 0)
        return fail(53, "call publish args allocation failed");
    a20_channel_call_args_t *call = (a20_channel_call_args_t *)(uintptr_t)args_page;
    uint8_t request[MSG_LEN], reply[MSG_LEN];
    for (uint32_t i = 0; i < MSG_LEN; i++) request[i] = (uint8_t)(0x40 + i);
    a20_handle_t reply_handle = A20_HANDLE_NULL;
    *call = (a20_channel_call_args_t){0};
    call->size = sizeof(*call);
    call->version = 1;
    call->channel = pair.endpoints[0];
    call->data = (uint64_t)(uintptr_t)request;
    call->data_len = MSG_LEN;
    call->handles = (uint64_t)(uintptr_t)&sent[0];
    call->handle_count = 1;
    call->reply_buf = (uint64_t)(uintptr_t)reply;
    call->reply_buf_len = MSG_LEN;
    call->reply_handle_buf = (uint64_t)(uintptr_t)&reply_handle;
    call->reply_handle_buf_count = 1;
    put_str("NATIVE_IPC: recv-txn final-args args ready\n");
    if (a20_vm_protect(args_page, 4096, A20_PROT_READ) < 0)
        return fail(54, "call publish args protection failed");
    put_str("NATIVE_IPC: recv-txn final-args invoking call\n");
    st = a20_syscall6(A20_SYS_channel_call, args_page, 0, 0, 0, 0, 0);
    put_str("NATIVE_IPC: recv-txn final-args call returned\n");
    if (st != -A20_ERR_FAULT)
        return fail(55, "read-only final args copy did not fault");
    if (reply_handle == A20_HANDLE_NULL ||
        a20_hdl_close(reply_handle) != -A20_ERR_BAD_HANDLE)
        return fail(56, "call reply handle survived final-copy rollback");
    a20_vm_unmap(args_page, 4096);
    a20_hdl_close(sent[0]);
    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    put_str("NATIVE_IPC: recv-txn final-args PASS\n");
    return 0;
}

/* The dedicated wake-race kernel build pre-wakes the caller after a local
 * parked receiver is selected but immediately before donate commit. The
 * channel must still wake that receiver, let it consume the queued request,
 * and leave the reply available after channel_call reports interruption. */
static int test_donate_pre_woken_fallback(void)
{
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK)
        return fail(57, "donate wake race channel setup failed");

    if (start_echo_server(&g_donate_test_ctx, pair.endpoints[1], 8,
                          &g_donate_test_stack) ==
        A20_HANDLE_NULL)
        return fail(58, "donate wake race server start failed");

    uint8_t request[MSG_LEN] = {
        'D', 'N', 'O', 'W', 'A', 'K', 'E', '1'
    };
    uint8_t reply[MSG_LEN];
    uint32_t reply_handle_count = 0;
    int saw_pre_woken = 0;
    for (uint32_t attempt = 0; attempt < 8; attempt++) {
        uint32_t reply_len = MSG_LEN;
        a20_status_t st = a20_channel_call(pair.endpoints[0], request,
                                           MSG_LEN, 0, 0, reply, &reply_len,
                                           0, 0);
        if (st == -A20_ERR_INTERRUPTED) {
            reply_len = MSG_LEN;
            st = a20_channel_recv(pair.endpoints[0], reply, &reply_len,
                                  0, &reply_handle_count);
            if (st < 0 || reply_len != MSG_LEN)
                return fail(59, "pre-woken request did not produce reply");
            saw_pre_woken = 1;
        } else if (st < 0 || reply_len != MSG_LEN) {
            return fail(60, "donate wake race RPC failed");
        }
        for (uint32_t i = 0; i < MSG_LEN; i++)
            if (reply[i] != request[i])
                return fail(61, "donate wake race reply mismatch");
    }
    if (!saw_pre_woken)
        return fail(62, "donor-prewoken injection was not observed");

    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    if (saw_pre_woken)
        put_str("NATIVE_IPC: donor-woken fallback PASS\n");
    return 0;
}

/* Exercise the earlier deferred-wake exit: channel_call has a stale reply
 * already queued, while its peer is parked waiting for the new request. The
 * call returns the stale reply immediately, but must still wake the server so
 * the request receives its own later reply. */
static int test_queued_reply_deferred_wake(void)
{
    static const uint8_t stale[] = {'S', 'T', 'A', 'L', 'E', '0', '0', '1'};
    static const uint8_t request[] = {'Q', 'R', 'E', 'P', 'L', 'Y', '0', '1'};
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK)
        return fail(63, "queued-reply channel setup failed");
    if (a20_channel_send(pair.endpoints[1], stale, sizeof(stale), 0, 0) < 0)
        return fail(64, "queued-reply stale message setup failed");

    if (start_echo_server(&g_queued_reply_ctx, pair.endpoints[1], 1,
                          &g_queued_reply_stack) ==
        A20_HANDLE_NULL)
        return fail(65, "queued-reply server start failed");
    uint32_t spins = 0;
    while (!__atomic_load_n(&g_queued_reply_ctx.ready, __ATOMIC_ACQUIRE) &&
           spins++ < 100000)
        a20_thread_yield();
    if (!__atomic_load_n(&g_queued_reply_ctx.ready, __ATOMIC_ACQUIRE))
        return fail(66, "queued-reply server did not reach recv");
    /* On this SMP1 fixture, yielding once returns only after the server has
     * linked its receive wait and parked. The kernel test hook independently
     * checks that a real waiter exists before accepting the scenario. */
    a20_thread_yield();

    uint8_t reply[MSG_LEN];
    uint32_t reply_len = MSG_LEN;
    uint32_t reply_handle_count = 0;
    a20_status_t st = a20_channel_call(pair.endpoints[0], request,
                                      sizeof(request), 0, 0, reply,
                                      &reply_len, 0, &reply_handle_count);
    if (st < 0 || reply_len != sizeof(stale))
        return fail(67, "queued-reply call did not return stale reply");
    for (uint32_t i = 0; i < sizeof(stale); i++)
        if (reply[i] != stale[i])
            return fail(68, "queued-reply stale payload mismatch");

    reply_len = MSG_LEN;
    st = a20_channel_recv(pair.endpoints[0], reply, &reply_len, 0,
                          &reply_handle_count);
    if (st < 0 || reply_len != sizeof(request))
        return fail(69, "queued-reply request was not serviced");
    for (uint32_t i = 0; i < sizeof(request); i++)
        if (reply[i] != request[i])
            return fail(70, "queued-reply request response mismatch");
    spins = 0;
    while (!__atomic_load_n(&g_queued_reply_ctx.done, __ATOMIC_ACQUIRE) &&
           spins++ < 100000)
        a20_thread_yield();
    if (!__atomic_load_n(&g_queued_reply_ctx.done, __ATOMIC_ACQUIRE))
        return fail(71, "queued-reply server did not exit");
    if (g_queued_reply_ctx.fail)
        return fail(71, "queued-reply server reported failure");

    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    put_str("NATIVE_IPC: queued-reply deferred wake PASS\n");
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    if (test_recv_transaction_rollback() != 0)
        return 91;
    if (argc > 1 && is_wake_race_arg(argv[1]) &&
        test_donate_pre_woken_fallback() != 0)
        return 92;
    if (argc > 1 && is_queued_reply_arg(argv[1]) &&
        test_queued_reply_deferred_wake() != 0)
        return 93;

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

    /* 5. Event delivery modes: level vs edge over a channel endpoint
     *    that already holds a message when the watch registers
     *    (09-native-abi-deepening Â§8 EventQ level mode). */
    {
        a20_channel_pair_t lpair;
        if (a20_channel_create(&lpair) != A20_OK)
            return fail(24, "level channel_create failed");
        uint8_t lmsg[8] = {1,2,3,4,5,6,7,8};
        if (a20_channel_send(lpair.endpoints[0], lmsg, sizeof(lmsg), 0, 0) < 0)
            return fail(25, "level channel_send failed");

        a20_handle_t levq, edgq;
        if (a20_event_queue_create(&levq) != A20_OK ||
            a20_event_queue_create(&edgq) != A20_OK)
            return fail(26, "event_queue_create failed");

        a20_time_t zero = { .secs = 0, .nsecs = 0 };
        a20_event_t evbuf;

        /* Edge watch registered after the message exists: no transition
         * happened since registration, so nothing may be delivered. */
        if (a20_event_watch(edgq, lpair.endpoints[1],
                            A20_EVENT_MASK(A20_EVENT_READABLE), 7) != A20_OK)
            return fail(27, "edge watch failed");
        st = a20_event_wait(edgq, zero, &evbuf);
        if (st != -A20_ERR_WOULD_BLOCK)
            return fail(28, "edge watch delivered without transition");

        /* Level watch over the same ready state: answered immediately
         * from current object state, carrying the watch's user_data. */
        if (a20_event_watch_level(levq, lpair.endpoints[1],
                                  A20_EVENT_MASK(A20_EVENT_READABLE), 42) != A20_OK)
            return fail(29, "level watch failed");
        st = a20_event_wait(levq, zero, &evbuf);
        if (st < 0)
            return fail(30, "level watch did not answer readiness");
        if (!(evbuf.events & A20_EVENT_MASK(A20_EVENT_READABLE)) ||
            evbuf.user_data != 42)
            return fail(31, "level event payload mismatch");

        a20_hdl_close(levq);
        a20_hdl_close(edgq);
        a20_hdl_close(lpair.endpoints[0]);
        a20_hdl_close(lpair.endpoints[1]);
    }

    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);

    put_str("NATIVE_IPC: PASS\n");
    return 0;
}
