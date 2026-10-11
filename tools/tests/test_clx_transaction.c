/* Exercise the production serial window against a bounded reference set,
 * including wrap, out-of-order delivery and delayed retired requests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../kernel/cluster/transaction.h"

int main(void) {
    a20_clx_window_t w = {0};
    assert(a20_clx_window_test(&w, 0) == A20_CLX_TX_STALE);
    assert(a20_clx_window_test(&w, A20_CLX_CLOSE_CALLER) == A20_CLX_TX_STALE);
    a20_clx_window_commit(&w, 0xfffffff0u);
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t next = w.high + 1;
        if (!next || next == A20_CLX_CLOSE_CALLER)
            next = 1;
        assert(a20_clx_window_test(&w, next) == A20_CLX_TX_NEW);
        a20_clx_window_commit(&w, next);
        assert(a20_clx_window_test(&w, next) == A20_CLX_TX_DUPLICATE);
        assert(a20_clx_window_test(&w, next - 64) == A20_CLX_TX_STALE);
    }
    w = (a20_clx_window_t){0};
    a20_clx_window_commit(&w, 100);
    a20_clx_window_commit(&w, 102);
    assert(a20_clx_window_test(&w, 101) == A20_CLX_TX_NEW);
    a20_clx_window_commit(&w, 101);
    assert(a20_clx_window_test(&w, 101) == A20_CLX_TX_DUPLICATE);
    a20_clx_window_commit(&w, 1000);
    assert(a20_clx_window_test(&w, 102) == A20_CLX_TX_STALE);
    uint8_t session[A20_CLX_SESSION_BYTES];
    a20_clx_session_encode(session, 0x123456789abcdef0ULL, 5);
    assert(a20_clx_session_matches(session, sizeof(session),
                                   0x123456789abcdef0ULL, 5));
    assert(!a20_clx_session_matches(session, sizeof(session),
                                    0x123456789abcdef0ULL, 6));
    assert(!a20_clx_session_matches(session, sizeof(session) - 1,
                                    0x123456789abcdef0ULL, 5));
    assert(!a20_clx_session_matches(NULL, sizeof(session), 1, 5));
    puts("test_clx_transaction: wrap, reorder, retired calls and session "
         "binding passed");
    return 0;
}
