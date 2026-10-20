/*
 * ufs_close_token.h — bounded open-token registry for the UFSD close-token
 * idempotency protocol (docs/fs/ufs-close-token.md).
 *
 * A token identifies exactly one open instance of one inode on one mount
 * channel. Issue happens at OPEN, consumption at the first accepted CLOSE;
 * the token then moves to a bounded accepted table and remains there until
 * the client confirms the CLOSE ACK. The table records acceptance only:
 * orphan-reap retries stay on the existing SYNC / mount-recovery paths.
 *
 * Header-only (static inline) so the vnfs backend and the host unit test
 * share one implementation, mirroring the vfile_lifecycle_c.h idiom.
 */
#ifndef UFS_CLOSE_TOKEN_H
#define UFS_CLOSE_TOKEN_H

#include <stdint.h>

#define UFS_CLOSE_TOKEN_LIVE_MAX 512u
#define UFS_CLOSE_TOKEN_RECENT_MAX 64u

/* Lookup verdicts for a CLOSE carrying a token. */
#define UFS_CLOSE_TOKEN_UNKNOWN 0 /* garbage close: -ENOENT */
#define UFS_CLOSE_TOKEN_LIVE 1    /* matches a live open: consume it */
#define UFS_CLOSE_TOKEN_ACCEPTED 2 /* accepted CLOSE: replay returns success */
#define UFS_CLOSE_TOKEN_REPLAY UFS_CLOSE_TOKEN_ACCEPTED
#define UFS_EAGAIN 11
#define UFS_EINVAL 22

typedef struct {
    uint64_t token; /* 0 = empty slot */
    uint64_t ino;
} ufs_close_token_live_t;

typedef struct {
    uint64_t token; /* 0 = empty slot */
    uint64_t tick;  /* accepted-order counter */
    uint64_t ino;   /* replay is valid only for the original inode */
} ufs_close_token_recent_t;

typedef struct {
    uint64_t epoch;  /* per-boot nonce; 0 = not initialized */
    uint64_t serial; /* per-mount monotonic issue counter */
    uint64_t tick;   /* accepted-order counter */
    ufs_close_token_live_t live[UFS_CLOSE_TOKEN_LIVE_MAX];
    ufs_close_token_recent_t recent[UFS_CLOSE_TOKEN_RECENT_MAX];
} ufs_close_token_table_t;

static inline uint64_t ufs_close_token_mix(uint64_t epoch, uint64_t serial)
{
    uint64_t x = epoch ^ (serial * 0x9e3779b97f4a7c15ull);
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x; /* bijective in serial for a fixed epoch; 0 is skipped by issue */
}

static inline int ufs_close_token_can_issue(const ufs_close_token_table_t *t)
{
    if (!t->epoch || t->serial == UINT64_MAX)
        return 0;
    int free_slot = 0;
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++)
        free_slot |= t->live[i].token == 0;
    if (!free_slot)
        return 0;
    uint64_t next = t->serial + 1;
    return ufs_close_token_mix(t->epoch, next) != 0 || next < UINT64_MAX;
}

static inline void ufs_close_token_init(ufs_close_token_table_t *t,
                                        uint64_t epoch)
{
    uint64_t i;
    t->epoch = epoch; /* zero means clock unavailable; issue then fails closed */
    t->serial = 0;
    t->tick = 0;
    for (i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++) {
        t->live[i].token = 0;
        t->live[i].ino = 0;
    }
    for (i = 0; i < UFS_CLOSE_TOKEN_RECENT_MAX; i++) {
        t->recent[i].token = 0;
        t->recent[i].tick = 0;
        t->recent[i].ino = 0;
    }
}

/* Issue a nonzero token for one accepted OPEN.  Returns 0 when the live
 * table is full; the caller then answers with token 0 and that open keeps
 * the legacy ambiguous-close behavior. */
static inline uint64_t ufs_close_token_issue(ufs_close_token_table_t *t,
                                             uint64_t ino)
{
    if (!ufs_close_token_can_issue(t))
        return 0;
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++) {
        if (!t->live[i].token) {
            uint64_t token;
            /* The mixer is bijective in serial for a fixed epoch. Never wrap
             * the serial; skip its single possible zero result, preserving
             * token 0 as the legacy/no-token marker. */
            do {
                if (t->serial == UINT64_MAX)
                    return 0;
                token = ufs_close_token_mix(t->epoch, ++t->serial);
            } while (!token);
            t->live[i].token = token;
            t->live[i].ino = ino;
            return t->live[i].token;
        }
    }
    return 0;
}

static inline int ufs_close_token_lookup(const ufs_close_token_table_t *t,
                                         uint64_t ino, uint64_t token)
{
    uint64_t i;
    if (!token)
        return UFS_CLOSE_TOKEN_UNKNOWN;
    for (i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++)
        if (t->live[i].token == token)
            return t->live[i].ino == ino ? UFS_CLOSE_TOKEN_LIVE
                                         : UFS_CLOSE_TOKEN_UNKNOWN;
    for (i = 0; i < UFS_CLOSE_TOKEN_RECENT_MAX; i++)
        if (t->recent[i].token == token)
            return t->recent[i].ino == ino ? UFS_CLOSE_TOKEN_REPLAY
                                           : UFS_CLOSE_TOKEN_UNKNOWN;
    return UFS_CLOSE_TOKEN_UNKNOWN;
}

static inline int ufs_close_token_can_consume(const ufs_close_token_table_t *t)
{
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_RECENT_MAX; i++)
        if (!t->recent[i].token)
            return 1;
    return 0;
}

/* Record accepted CLOSE without evicting an unconfirmed result. */
static inline int ufs_close_token_consume(ufs_close_token_table_t *t,
                                          uint64_t ino, uint64_t token)
{
    uint64_t live_index = UFS_CLOSE_TOKEN_LIVE_MAX;
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++) {
        if (t->live[i].token == token) {
            if (t->live[i].ino != ino)
                return -UFS_EINVAL;
            live_index = i;
            break;
        }
    }
    if (live_index == UFS_CLOSE_TOKEN_LIVE_MAX)
        return -UFS_EINVAL;
    if (!ufs_close_token_can_consume(t))
        return -UFS_EAGAIN;
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_RECENT_MAX; i++) {
        if (!t->recent[i].token) {
            t->live[live_index] = (ufs_close_token_live_t){0};
            t->recent[i] = (ufs_close_token_recent_t){
                .token = token, .tick = ++t->tick, .ino = ino
            };
            return 0;
        }
    }
    return -UFS_EAGAIN;
}

/* CONFIRM retires one accepted tombstone. Unknown CONFIRM is idempotent; a
 * token known on another inode is rejected. */
static inline int ufs_close_token_confirm(ufs_close_token_table_t *t,
                                          uint64_t ino, uint64_t token)
{
    if (!token)
        return 0;
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++) {
        if (t->live[i].token == token)
            return t->live[i].ino == ino ? -UFS_EAGAIN : -UFS_EINVAL;
    }
    for (uint64_t i = 0; i < UFS_CLOSE_TOKEN_RECENT_MAX; i++) {
        if (t->recent[i].token == token) {
            if (t->recent[i].ino != ino)
                return -UFS_EINVAL;
            t->recent[i] = (ufs_close_token_recent_t){0};
            return 0;
        }
    }
    return 0;
}

/* Drop live tokens of an inode whose open count reached zero, so a leaked
 * entry cannot outlive the open instance it named. */
static inline void ufs_close_token_forget_ino(ufs_close_token_table_t *t,
                                              uint64_t ino)
{
    uint64_t i;
    for (i = 0; i < UFS_CLOSE_TOKEN_LIVE_MAX; i++)
        if (t->live[i].token && t->live[i].ino == ino) {
            t->live[i].token = 0;
            t->live[i].ino = 0;
        }
}

#endif /* UFS_CLOSE_TOKEN_H */
