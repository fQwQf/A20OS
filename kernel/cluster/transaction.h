#ifndef _CLUSTER_TRANSACTION_H
#define _CLUSTER_TRANSACTION_H

#include "cluster/uart.h"

/* Negotiated reliable CALL/REPLY payload prefix. Never passed to services.
 * The receiver epoch prevents a delayed call from executing after reboot. */
#define A20_CLX_SESSION_BYTES 16u
#define A20_CLX_REPLY_PREFIX 20u /* session pair followed by u32 status */
#define A20_CLX_RETRY_MS 250u
#define A20_CLX_RETRY_MAX 8u
#define A20_CLX_RETAIN_MS 35000u

typedef struct a20_clx_window {
    uint32_t high;
    uint64_t seen;
} a20_clx_window_t;

enum { A20_CLX_TX_NEW = 0, A20_CLX_TX_DUPLICATE = 1, A20_CLX_TX_STALE = 2 };

/* RFC 1982 serial arithmetic. The bounded retry lifetime must cover fewer
 * than 2^31 transaction numbers. Old, unseen calls fail rather than execute. */
static inline int a20_clx_window_test(const a20_clx_window_t *w,
                                      uint32_t txid) {
    if (!txid || txid == A20_CLX_CLOSE_CALLER)
        return A20_CLX_TX_STALE;
    if (!w->seen || (int32_t)(txid - w->high) > 0)
        return A20_CLX_TX_NEW;
    uint32_t age = w->high - txid;
    if (age >= 64)
        return A20_CLX_TX_STALE;
    return (w->seen & (1ULL << age)) ? A20_CLX_TX_DUPLICATE : A20_CLX_TX_NEW;
}

/* Call only after reserving execution and reply storage. */
static inline void a20_clx_window_commit(a20_clx_window_t *w, uint32_t txid) {
    if (!w->seen) {
        w->high = txid;
        w->seen = 1;
    } else if ((int32_t)(txid - w->high) > 0) {
        uint32_t distance = txid - w->high;
        w->seen = distance >= 64 ? 1 : (w->seen << distance) | 1;
        w->high = txid;
    } else if (w->high - txid < 64) {
        w->seen |= 1ULL << (w->high - txid);
    }
}

static inline void a20_clx_session_encode(uint8_t *p, uint64_t source,
                                          uint64_t destination) {
    a20_clx_put_le64(p, source);
    a20_clx_put_le64(p + 8, destination);
}

static inline int a20_clx_session_matches(const uint8_t *p, uint32_t len,
                                          uint64_t source,
                                          uint64_t destination) {
    return p && len >= A20_CLX_SESSION_BYTES && source && destination &&
           a20_clx_get_le64(p) == source &&
           a20_clx_get_le64(p + 8) == destination;
}

#endif
