/*
 * Host ABI harness for the UXFS RPC engine (UXFS_RPC_STATE_IMPL=rust).
 *
 * Drives kernel/fs/uxfs/uxfs_state.rs through the exact callback table the
 * kernel adapter installs, with a scripted channel, and checks the loop's
 * externally visible contract: admission, deadline expiry, send retry,
 * stale-reply dropping, payload bounds and response copy.  The Rust unit tests
 * in the module cover the same state machine; this proves the C ABI layout and
 * the callback protocol.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UFS_REQ_MAGIC 0x55465352u
#define UFS_RESP_MAGIC 0x55465350u
#define UFS_OP_INIT 1u
#define UFS_OP_LOOKUP 2u
#define UFS_OP_CLOSE 16u

typedef struct ReqHdr {
    uint32_t magic, version, opcode, req_id;
    uint64_t ino, arg0, arg1;
    uint32_t name_len, payload_len;
} ReqHdr;

typedef struct RespHdr {
    uint32_t magic, opcode, req_id;
    int32_t status;
    uint64_t out0, out1, out2;
    uint32_t payload_len, pad;
} RespHdr;

typedef struct Ops {
    uint64_t (*now_ns)(void);
    void (*sleep_tick)(void);
    int (*try_lock)(void *ctx);
    void (*unlock)(void *ctx);
    int (*send)(void *ctx, const uint8_t *msg, uint32_t len, uint64_t deadline, int nonblock);
    int (*recv)(void *ctx, uint8_t *buf, uint32_t cap, uint64_t deadline, int nonblock, uint32_t *out_len);
    int (*has_ep)(void *ctx);
    int (*shutdown_started)(void *ctx);
} Ops;

extern int a20_uxfs_rpc_run(void *ctx, const Ops *ops, const ReqHdr *req,
                            const uint8_t *msg, uint32_t msg_len, uint8_t *rx,
                            uint32_t rx_cap, RespHdr *resp, void *resp_payload,
                            uint32_t resp_cap, uint32_t *resp_payload_len,
                            uint64_t deadline_ns, int *request_sent);

typedef struct Chan {
    int locked;
    int has_ep;
    int shutdown;
    uint64_t now;
    uint64_t sleep_cost;
    int send_would_block;
    int send_calls;
    int recv_calls;
    uint8_t frames[8][256];
    uint32_t frame_len[8];
    int nframes, cur;
} Chan;

static Chan g_chan;

static uint64_t now_ns(void) { return g_chan.now; }
static void sleep_tick(void) { g_chan.now += g_chan.sleep_cost; }
static int try_lock(void *ctx) {
    (void)ctx;
    if (g_chan.locked) return 0;
    g_chan.locked = 1;
    return 1;
}
static void unlock(void *ctx) { (void)ctx; g_chan.locked = 0; }
static int send_cb(void *ctx, const uint8_t *msg, uint32_t len, uint64_t d, int nb) {
    (void)ctx; (void)msg; (void)len; (void)d; (void)nb;
    g_chan.send_calls++;
    if (g_chan.send_would_block > 0) { g_chan.send_would_block--; return 1; }
    return 0;
}
static int recv_cb(void *ctx, uint8_t *buf, uint32_t cap, uint64_t d, int nb, uint32_t *out_len) {
    (void)ctx; (void)d; (void)nb;
    g_chan.recv_calls++;
    g_chan.now += 1000000; /* a receive iteration costs time */
    if (g_chan.cur >= g_chan.nframes) return -1; /* WOULD_BLOCK */
    uint32_t n = g_chan.frame_len[g_chan.cur];
    if (n > cap) return -3;
    memcpy(buf, g_chan.frames[g_chan.cur], n);
    *out_len = n;
    g_chan.cur++;
    return (int)n;
}
static int has_ep(void *ctx) { (void)ctx; return g_chan.has_ep; }
static int shutdown_started(void *ctx) { (void)ctx; return g_chan.shutdown; }

static const Ops g_ops = {
    now_ns, sleep_tick, try_lock, unlock, send_cb, recv_cb, has_ep, shutdown_started
};

static ReqHdr req_of(uint32_t op, uint32_t id) {
    ReqHdr r;
    memset(&r, 0, sizeof(r));
    r.magic = UFS_REQ_MAGIC; r.version = 1; r.opcode = op; r.req_id = id; r.ino = 1;
    return r;
}

static void push_resp(uint32_t op, uint32_t id, int32_t status, const void *pl, uint32_t plen) {
    Chan *c = &g_chan;
    RespHdr h;
    memset(&h, 0, sizeof(h));
    h.magic = UFS_RESP_MAGIC; h.opcode = op; h.req_id = id; h.status = status; h.payload_len = plen;
    memcpy(c->frames[c->nframes], &h, sizeof(h));
    if (plen && pl) memcpy(c->frames[c->nframes] + sizeof(h), pl, plen);
    memset(c->frames[c->nframes] + sizeof(h) + (pl ? plen : 0), 0,
           plen - (pl ? plen : 0));
    c->frame_len[c->nframes] = (uint32_t)(sizeof(h) + plen);
    c->nframes++;
}

static int g_fail;
static void check(const char *what, int cond, long got, long want) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, got, want);
        g_fail = 1;
    }
}

static int run(uint32_t op, uint32_t id, uint64_t deadline,
               RespHdr *resp, uint8_t *payload, uint32_t *plen, int *sent) {
    ReqHdr r = req_of(op, id);
    uint8_t msg[16] = {0};
    uint8_t rx[512];
    *plen = 0; *sent = 0;
    return a20_uxfs_rpc_run(&g_chan, &g_ops, &r, msg, sizeof(msg), rx, sizeof(rx),
                            resp, payload, 64, plen, deadline, sent);
}

static void reset(void) {
    memset(&g_chan, 0, sizeof(g_chan));
    g_chan.has_ep = 1;
    g_chan.sleep_cost = 2000000;
}

int main(void) {
    RespHdr resp;
    uint8_t payload[64];
    uint32_t plen = 0;
    int sent = 0;
    int rc;

    /* 1. Matching reply with payload, lock released. */
    reset();
    push_resp(UFS_OP_INIT, 7, 0, "abc", 3);
    rc = run(UFS_OP_INIT, 7, 0, &resp, payload, &plen, &sent);
    check("success rc", rc == 0, rc, 0);
    check("success plen", plen == 3, plen, 3);
    check("success payload", memcmp(payload, "abc", 3) == 0, 0, 0);
    check("success req_id", resp.req_id == 7, resp.req_id, 7);
    check("success sent", sent == 1, sent, 1);
    check("success unlocked", !g_chan.locked, g_chan.locked, 0);

    /* 2. Stale reply becomes a deadline timeout, not a false success. */
    reset();
    push_resp(UFS_OP_INIT, 6, 0, NULL, 0);
    rc = run(UFS_OP_INIT, 7, 5000000, &resp, payload, &plen, &sent);
    check("stale rc", rc == -110, rc, -110);
    check("stale sent", sent == 1, sent, 1);
    check("stale recv retried", g_chan.recv_calls > 1, g_chan.recv_calls, 2);

    /* 3. Negative server status is returned verbatim. */
    reset();
    push_resp(UFS_OP_INIT, 7, -13, NULL, 0);
    rc = run(UFS_OP_INIT, 7, 0, &resp, payload, &plen, &sent);
    check("status rc", rc == -13, rc, -13);

    /* 4. Send queue backpressure retries before succeeding. */
    reset();
    g_chan.send_would_block = 3;
    push_resp(UFS_OP_INIT, 1, 0, NULL, 0);
    rc = run(UFS_OP_INIT, 1, 100000000, &resp, payload, &plen, &sent);
    check("backpressure rc", rc == 0, rc, 0);
    check("backpressure send calls", g_chan.send_calls == 4, g_chan.send_calls, 4);

    /* 5. Send queue never drains: deadline timeout without a send marked. */
    reset();
    g_chan.send_would_block = 1000;
    rc = run(UFS_OP_INIT, 1, 5000000, &resp, payload, &plen, &sent);
    check("send deadline rc", rc == -110, rc, -110);
    check("send deadline sent", sent == 0, sent, 0);

    /* 6. No endpoint is ESHUTDOWN even for INIT. */
    reset();
    g_chan.has_ep = 0;
    rc = run(UFS_OP_INIT, 1, 0, &resp, payload, &plen, &sent);
    check("no ep rc", rc == -108, rc, -108);

    /* 7. Shutdown admits only INIT/SHUTDOWN/CLOSE. */
    reset();
    g_chan.shutdown = 1;
    rc = run(UFS_OP_LOOKUP, 1, 0, &resp, payload, &plen, &sent);
    check("shutdown lookup rc", rc == -108, rc, -108);
    check("shutdown lookup not sent", sent == 0, sent, 0);
    reset();
    g_chan.shutdown = 1;
    push_resp(UFS_OP_CLOSE, 2, 0, NULL, 0);
    rc = run(UFS_OP_CLOSE, 2, 0, &resp, payload, &plen, &sent);
    check("shutdown close rc", rc == 0, rc, 0);

    /* 8. Oversized payload is a protocol error. */
    reset();
    push_resp(UFS_OP_INIT, 1, 0, NULL, 70);
    rc = run(UFS_OP_INIT, 1, 0, &resp, payload, &plen, &sent);
    check("oversize rc", rc == -5, rc, -5);

    /* 9. Contended lock with an expired deadline. */
    reset();
    g_chan.locked = 1;
    rc = run(UFS_OP_INIT, 1, 5000000, &resp, payload, &plen, &sent);
    check("lock deadline rc", rc == -110, rc, -110);
    check("lock deadline still locked", g_chan.locked == 1, g_chan.locked, 1);

    if (g_fail) return 1;
    printf("uxfs rpc state host ABI harness PASS (9 scenarios)\n");
    return 0;
}
