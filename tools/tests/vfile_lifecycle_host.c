#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "fs/vfs/vfile_lifecycle_c.h"

extern int a20_vfile_close_begin(uint32_t *state);
extern int a20_vfile_close_begin_token(uint32_t *state, int token_capable);
extern int a20_vfile_close_finish(uint32_t *state, int status, int sent,
                                  int acked, int retry_safe);
extern int a20_vfile_close_is_done(const uint32_t *state);

int main(void)
{
    uint32_t state = 0;
    int calls = 0;

    /* A writeback failure happens before CLOSE enqueue and can be retried. */
    assert(a20_vfile_close_begin(&state) == 0);
    calls++;
    assert(a20_vfile_close_finish(&state, -5, 0, 0, 1) == -5);
    assert(a20_vfile_close_begin(&state) == 0);

    /* Once sent, a lost reply must not blindly consume a second OPEN ref. */
    calls++;
    assert(a20_vfile_close_finish(&state, -5, 1, 0, 0) == -5);
    assert(a20_vfile_close_begin(&state) == -5);
    assert(!a20_vfile_close_is_done(&state));
    assert(calls == 2);

    /* A fresh vfile with a matching successful ACK is terminal exactly once. */
    state = 0;
    assert(a20_vfile_close_begin(&state) == 0);
    calls++;
    assert(a20_vfile_close_finish(&state, 0, 1, 1, 0) == 0);
    assert(a20_vfile_close_is_done(&state));
    assert(a20_vfile_close_begin(&state) == -114);
    assert(calls == 3);

    /* Run the same transitions through the C reference and compare results. */
    uint32_t cstate = 0, rstate = 0;
    assert(a20_vfile_close_begin_c(&cstate) == a20_vfile_close_begin(&rstate));
    int cr = a20_vfile_close_finish_c(&cstate, -5, 0, 0, 1);
    int rr = a20_vfile_close_finish(&rstate, -5, 0, 0, 1);
    assert(cr == rr && cstate == rstate);
    assert(a20_vfile_close_begin_c(&cstate) == a20_vfile_close_begin(&rstate));
    cr = a20_vfile_close_finish_c(&cstate, 0, 1, 1, 0);
    rr = a20_vfile_close_finish(&rstate, 0, 1, 1, 0);
    assert(cr == rr && cstate == rstate);
    cstate = 0;
    rstate = 0;
    assert(a20_vfile_close_begin_c(&cstate) == a20_vfile_close_begin(&rstate));
    cr = a20_vfile_close_finish_c(&cstate, -5, 1, 0, 0);
    rr = a20_vfile_close_finish(&rstate, -5, 1, 0, 0);
    assert(cr == rr && cstate == rstate);
    assert(a20_vfile_close_begin_c(&cstate) == a20_vfile_close_begin(&rstate));
    assert(cstate == rstate);

    /* Token-aware begin: AMBIGUOUS->RUNNING->DONE with the capability,
     * sticky -EIO without. Run both orderings through the C reference and
     * the Rust FFI and compare every result and state. */
    cstate = 0;
    rstate = 0;
    assert(a20_vfile_close_begin_token_c(&cstate, 1) ==
           a20_vfile_close_begin_token(&rstate, 1));
    cr = a20_vfile_close_finish_c(&cstate, -5, 1, 0, 0);
    rr = a20_vfile_close_finish(&rstate, -5, 1, 0, 0);
    assert(cr == rr && cstate == rstate && cstate == 2 /* AMBIGUOUS */);
    assert(a20_vfile_close_begin_token_c(&cstate, 0) ==
           a20_vfile_close_begin_token(&rstate, 0));
    assert(cstate == rstate && cstate == 2); /* no capability: sticky -EIO */
    assert(a20_vfile_close_begin_c(&cstate) == a20_vfile_close_begin(&rstate));
    assert(cstate == rstate && cstate == 2); /* legacy begin: sticky -EIO */
    assert(a20_vfile_close_begin_token_c(&cstate, 1) ==
           a20_vfile_close_begin_token(&rstate, 1));
    assert(cstate == rstate && cstate == 4 /* RUNNING */);
    cr = a20_vfile_close_finish_c(&cstate, 0, 1, 1, 0);
    rr = a20_vfile_close_finish(&rstate, 0, 1, 1, 0);
    assert(cr == rr && cstate == rstate && cstate == 3 /* DONE */);
    assert(a20_vfile_close_is_done_c(&cstate) == a20_vfile_close_is_done(&rstate));
    assert(a20_vfile_close_begin_token_c(&cstate, 1) ==
           a20_vfile_close_begin_token(&rstate, 1));
    assert(cstate == rstate);

    /* A token retry that is itself sent-but-un-acked returns to AMBIGUOUS
     * and remains retryable; C and Rust must agree on each step. */
    cstate = 2;
    rstate = 2;
    assert(a20_vfile_close_begin_token_c(&cstate, 1) ==
           a20_vfile_close_begin_token(&rstate, 1));
    cr = a20_vfile_close_finish_c(&cstate, -5, 1, 0, 0);
    rr = a20_vfile_close_finish(&rstate, -5, 1, 0, 0);
    assert(cr == rr && cstate == rstate && cstate == 2);
    assert(a20_vfile_close_begin_token_c(&cstate, 1) ==
           a20_vfile_close_begin_token(&rstate, 1));
    cr = a20_vfile_close_finish_c(&cstate, 0, 1, 1, 0);
    rr = a20_vfile_close_finish(&rstate, 0, 1, 1, 0);
    assert(cr == rr && cstate == rstate && cstate == 3);
    puts("vfile close lifecycle C->Rust PASS");
    return 0;
}
