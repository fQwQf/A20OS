/*
 * Host unit test for the UFSD close-token registry shared by the vnfs
 * backend (user/svc/ufs_close_token.h).  Covers the protocol contract of
 * docs/fs/ufs-close-token.md: issue at OPEN, exactly-once
 * consume at CLOSE, idempotent replay, unknown-token rejection, and the
 * bounded recent-closed LRU.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "fs/ufs_proto.h"
#include "ufs_backends.h"
#include "ufs_close_token.h"

static int64_t mock_open(uint64_t ino, uint64_t *token_out)
{ (void)ino; (void)token_out; return 0; }
static int64_t mock_close(uint64_t ino, uint64_t token)
{ (void)ino; (void)token; return 0; }
static int64_t mock_confirm(uint64_t ino, uint64_t token)
{ (void)ino; (void)token; return 0; }

extern void ufs_close_token_host_reset(uint64_t epoch);
extern uint64_t ufs_close_token_host_open(uint64_t ino);
extern int64_t ufs_close_token_host_close(uint64_t ino, uint64_t token);
extern int64_t ufs_close_token_host_confirm(uint64_t ino, uint64_t token);
extern uint32_t ufs_close_token_host_backend_closes(void);

int main(void)
{
    /* Wire ABI used by the production kernel and UFSD dispatchers. */
    _Static_assert(sizeof(ufs_req_hdr_t) == 48, "request ABI changed");
    _Static_assert(sizeof(ufs_resp_hdr_t) == 48, "response ABI changed");
    _Static_assert(offsetof(ufs_req_hdr_t, arg0) == 24, "arg0 wire offset changed");
    _Static_assert(offsetof(ufs_resp_hdr_t, out2) == 32, "out2 wire offset changed");
    ufs_req_hdr_t init = { .opcode = UFS_OP_INIT,
                           .arg0 = UFS_CLOSE_CONFIRM_CAPABILITY };
    ufs_resp_hdr_t init_reply = { .out1 = UFS_CLOSE_CONFIRM_CAPABILITY };
    ufs_resp_hdr_t open_reply = { .out2 = 0x12345678 };
    ufs_req_hdr_t close_req = { .opcode = UFS_OP_CLOSE,
                                .arg0 = open_reply.out2 };
    assert(init.arg0 == init_reply.out1);
    assert(UFS_CLOSE_TOKEN_CAPABILITY != UFS_CLOSE_CONFIRM_CAPABILITY);
    assert(close_req.arg0 == open_reply.out2);

    ufs_backend_t mock_backend = {
        .open = mock_open, .close = mock_close, .close_confirm = mock_confirm,
    };
    int tokens_enabled = 0, confirm_enabled = 0;
    assert(ufs_backend_lifetime_negotiate(&mock_backend, init.arg0,
             &tokens_enabled, &confirm_enabled) == UFS_CLOSE_CONFIRM_CAPABILITY);
    assert(tokens_enabled && confirm_enabled);
    assert(ufs_backend_lifetime_negotiate(&mock_backend,
             UFS_CLOSE_TOKEN_CAPABILITY, &tokens_enabled,
             &confirm_enabled) == UFS_LIFETIME_CAPABILITY);
    assert(!tokens_enabled && !confirm_enabled);
    mock_backend.close_confirm = NULL;
    assert(ufs_backend_lifetime_negotiate(&mock_backend, init.arg0,
             &tokens_enabled, &confirm_enabled) == UFS_LIFETIME_CAPABILITY);
    assert(!tokens_enabled && !confirm_enabled);

    /* Cross C/Rust same-wirecase: both implementations issue the same token,
     * retain acceptance across a lost CLOSE ACK, then retire it only after
     * CLOSE_CONFIRM. */
    ufs_close_token_table_t c_wire;
    ufs_close_token_init(&c_wire, 0x987654321ull);
    uint64_t c_token = ufs_close_token_issue(&c_wire, 41);
    ufs_close_token_host_reset(0x987654321ull);
    uint64_t rust_token = ufs_close_token_host_open(41);
    assert(c_token != 0 && rust_token == c_token);
    assert(ufs_close_token_lookup(&c_wire, 41, rust_token) == UFS_CLOSE_TOKEN_LIVE);
    assert(ufs_close_token_host_close(41, c_token) == 0);
    assert(ufs_close_token_consume(&c_wire, 41, c_token) == 0);
    assert(ufs_close_token_host_close(41, c_token) == 1);
    assert(ufs_close_token_host_backend_closes() == 1);
    assert(ufs_close_token_lookup(&c_wire, 41, c_token) == UFS_CLOSE_TOKEN_REPLAY);
    assert(ufs_close_token_confirm(&c_wire, 41, c_token) == 0);
    assert(ufs_close_token_host_confirm(41, c_token) == 0);
    assert(ufs_close_token_host_confirm(41, c_token) == 0);
    assert(ufs_close_token_lookup(&c_wire, 41, c_token) == UFS_CLOSE_TOKEN_UNKNOWN);

    ufs_close_token_table_t t = {0};

    /* Uninitialized table issues nothing. */
    assert(ufs_close_token_issue(&t, 7) == 0);
    ufs_close_token_init(&t, 0);
    assert(ufs_close_token_issue(&t, 7) == 0); /* no epoch, no tokens */

    ufs_close_token_init(&t, 0x1234);

    /* Tokens are nonzero and distinct per open instance. */
    uint64_t tok_a = ufs_close_token_issue(&t, 7);
    uint64_t tok_b = ufs_close_token_issue(&t, 7);
    uint64_t tok_c = ufs_close_token_issue(&t, 9);
    assert(tok_a && tok_b && tok_c);
    assert(tok_a != tok_b && tok_a != tok_c && tok_b != tok_c);

    /* A live token matches only its own inode. */
    assert(ufs_close_token_lookup(&t, 7, tok_a) == UFS_CLOSE_TOKEN_LIVE);
    assert(ufs_close_token_lookup(&t, 9, tok_a) == UFS_CLOSE_TOKEN_UNKNOWN);

    /* Consume: accepted state remains until explicit CLOSE_CONFIRM. */
    assert(ufs_close_token_consume(&t, 7, tok_a) == 0);
    assert(ufs_close_token_lookup(&t, 7, tok_a) == UFS_CLOSE_TOKEN_REPLAY);
    assert(ufs_close_token_lookup(&t, 8, tok_a) == UFS_CLOSE_TOKEN_UNKNOWN);
    assert(ufs_close_token_lookup(&t, 7, tok_b) == UFS_CLOSE_TOKEN_LIVE);

    /* Unknown token stays an error; token 0 is never a token. */
    assert(ufs_close_token_lookup(&t, 7, 0xdeadbeef) == UFS_CLOSE_TOKEN_UNKNOWN);
    assert(ufs_close_token_lookup(&t, 7, 0) == UFS_CLOSE_TOKEN_UNKNOWN);

    assert(ufs_close_token_confirm(&t, 7, tok_b) == -UFS_EAGAIN);
    assert(ufs_close_token_confirm(&t, 9, tok_a) == -UFS_EINVAL);
    assert(ufs_close_token_confirm(&t, 7, tok_a) == 0);
    assert(ufs_close_token_confirm(&t, 7, tok_a) == 0); /* unknown idempotent */
    assert(ufs_close_token_lookup(&t, 7, tok_a) == UFS_CLOSE_TOKEN_UNKNOWN);

    /* Forget: releasing the open instance drops its live token. */
    ufs_close_token_forget_ino(&t, 7);
    assert(ufs_close_token_lookup(&t, 7, tok_b) == UFS_CLOSE_TOKEN_UNKNOWN);
    assert(ufs_close_token_lookup(&t, 9, tok_c) == UFS_CLOSE_TOKEN_LIVE);

    /* Live-table bound: exhaustion degrades to token 0 (legacy open). */
    ufs_close_token_table_t full;
    ufs_close_token_init(&full, 0x77);
    uint64_t issued = 0;
    for (uint64_t i = 1; i <= UFS_CLOSE_TOKEN_LIVE_MAX; i++) {
        assert(ufs_close_token_issue(&full, i) != 0);
        issued++;
    }
    assert(issued == UFS_CLOSE_TOKEN_LIVE_MAX);
    assert(ufs_close_token_issue(&full, 9999) == 0);

    /* Accepted outcomes do not evict: a full table refuses CLOSE before
     * consuming the live token, and confirm makes a slot available. */
    ufs_close_token_table_t full_recent;
    ufs_close_token_init(&full_recent, 0x99);
    uint64_t first = 0;
    for (uint64_t i = 1; i <= UFS_CLOSE_TOKEN_RECENT_MAX; i++) {
        uint64_t tok = ufs_close_token_issue(&full_recent, i);
        assert(tok != 0);
        if (i == 1) first = tok;
        assert(ufs_close_token_consume(&full_recent, i, tok) == 0);
    }
    assert(ufs_close_token_lookup(&full_recent, 1, first) == UFS_CLOSE_TOKEN_ACCEPTED);
    assert(!ufs_close_token_can_consume(&full_recent));
    uint64_t pending = ufs_close_token_issue(&full_recent, 1000);
    assert(pending != 0);
    assert(ufs_close_token_consume(&full_recent, 1000, pending) == -UFS_EAGAIN);
    assert(ufs_close_token_lookup(&full_recent, 1000, pending) == UFS_CLOSE_TOKEN_LIVE);
    assert(ufs_close_token_confirm(&full_recent, 1, first) == 0);
    assert(ufs_close_token_can_consume(&full_recent));
    assert(ufs_close_token_consume(&full_recent, 1000, pending) == 0);
    assert(ufs_close_token_lookup(&full_recent, 1000, pending) == UFS_CLOSE_TOKEN_ACCEPTED);

    /* Serial exhaustion fails closed rather than wrapping and reusing an
     * earlier token. */
    ufs_close_token_table_t exhausted;
    ufs_close_token_init(&exhausted, 0x123);
    exhausted.serial = UINT64_MAX;
    assert(ufs_close_token_issue(&exhausted, 1) == 0);

    /* Same serial stream with a different boot epoch never collides. */
    ufs_close_token_table_t boot_b;
    ufs_close_token_init(&boot_b, 0x43210000);
    uint64_t other_epoch = ufs_close_token_issue(&boot_b, 7);
    assert(other_epoch != tok_a);

    puts("ufs close token registry host test PASS");
    return 0;
}
