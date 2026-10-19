/*
 * Host ABI harness for the UXFS lifetime negotiation engine
 * (kernel/fs/uxfs/lifetime_state.rs).
 *
 * Exercises the exact struct layout and entry points uxfs.c calls, including
 * the C _Static_assert equivalent (sizeof/offsetof) and the capability
 * decision table.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Lifetime {
    uint32_t mode, shutdown_mode, close_token_mode, close_confirm_mode;
    uint64_t generation;
    void *server;
} Lifetime;

#define UFS_LIFETIME_CAPABILITY 0x5546534c49464532ull
#define UFS_SHUTDOWN_CAPABILITY 0x5546535348555432ull
#define UFS_CLOSE_TOKEN_CAPABILITY 0x55465343544f4b31ull
#define UFS_CLOSE_CONFIRM_CAPABILITY 0x55465343464d5231ull

extern void a20_uxfs_lifetime_reset(Lifetime *);
extern int a20_uxfs_lifetime_negotiated(const Lifetime *, uint64_t, void *);
extern void a20_uxfs_lifetime_apply_init(Lifetime *, uint64_t, uint64_t, uint64_t, void *);
extern int a20_uxfs_lifetime_is_v1(const Lifetime *);
extern int a20_uxfs_lifetime_is_v2(const Lifetime *);
extern int a20_uxfs_lifetime_shutdown_supported(const Lifetime *);
extern uint64_t a20_uxfs_lifetime_open_token(const Lifetime *, uint64_t);
extern int a20_uxfs_lifetime_close_needs_rpc(const Lifetime *);
extern int a20_uxfs_lifetime_close_v1_acked(const Lifetime *);

static int g_fail;
static void check(const char *what, long got, long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, got, want);
        g_fail = 1;
    }
}
static void check64(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %llx want %llx\n", what,
                (unsigned long long)got, (unsigned long long)want);
        g_fail = 1;
    }
}

int main(void) {
    check("sizeof", (long)sizeof(Lifetime), 32);
    check("gen offset", (long)__builtin_offsetof(Lifetime, generation), 16);
    check("server offset", (long)__builtin_offsetof(Lifetime, server), 24);

    Lifetime st;
    memset(&st, 0xAA, sizeof(st));

    /* Legacy peer: explicit v1, no token, CLOSE is a local no-op. */
    a20_uxfs_lifetime_apply_init(&st, 0, 0, 7, (void *)0x1000);
    check("legacy mode", st.mode, 1);
    check("legacy v1", a20_uxfs_lifetime_is_v1(&st), 1);
    check("legacy v2", a20_uxfs_lifetime_is_v2(&st), 0);
    check("legacy negotiated", a20_uxfs_lifetime_negotiated(&st, 7, (void *)0x1000), 1);
    check("legacy gen mismatch", a20_uxfs_lifetime_negotiated(&st, 8, (void *)0x1000), 0);
    check("legacy srv mismatch", a20_uxfs_lifetime_negotiated(&st, 7, (void *)0x2000), 0);
    check("legacy no rpc", a20_uxfs_lifetime_close_needs_rpc(&st), 0);
    check("legacy acked", a20_uxfs_lifetime_close_v1_acked(&st), 1);
    check64("legacy token", a20_uxfs_lifetime_open_token(&st, 0x44), 0);
    check("legacy shutdown", a20_uxfs_lifetime_shutdown_supported(&st), 0);

    /* v2 lifetime-only: no tokens/confirm, but CLOSE needs the wire RPC. */
    a20_uxfs_lifetime_apply_init(&st, UFS_LIFETIME_CAPABILITY, 0, 3, (void *)0x3000);
    check("lifetime mode", st.mode, 2);
    check("lifetime token mode", st.close_token_mode, 0);
    check("lifetime confirm", st.close_confirm_mode, 0);
    check("lifetime rpc", a20_uxfs_lifetime_close_needs_rpc(&st), 1);
    check("lifetime acked", a20_uxfs_lifetime_close_v1_acked(&st), 0);
    check64("lifetime token", a20_uxfs_lifetime_open_token(&st, 0x55), 0);

    /* Token-only capability stays token-disabled (not confirm-safe). */
    a20_uxfs_lifetime_apply_init(&st, UFS_CLOSE_TOKEN_CAPABILITY, 0, 3, (void *)0x3000);
    check("token-only mode", st.mode, 2);
    check("token-only token mode", st.close_token_mode, 0);
    check("token-only confirm", st.close_confirm_mode, 0);

    /* Confirm capability enables tokens, confirm retirement and shutdown. */
    a20_uxfs_lifetime_apply_init(&st, UFS_CLOSE_CONFIRM_CAPABILITY,
                                 UFS_SHUTDOWN_CAPABILITY, 4, (void *)0x4000);
    check("confirm mode", st.mode, 2);
    check("confirm token mode", st.close_token_mode, 1);
    check("confirm confirm", st.close_confirm_mode, 1);
    check("confirm shutdown", a20_uxfs_lifetime_shutdown_supported(&st), 1);
    check64("confirm token", a20_uxfs_lifetime_open_token(&st, 0x77), 0x77);
    check("confirm negotiated", a20_uxfs_lifetime_negotiated(&st, 4, (void *)0x4000), 1);

    /* Reset clears validity. */
    a20_uxfs_lifetime_reset(&st);
    check("reset mode", st.mode, 0);
    check("reset negotiated", a20_uxfs_lifetime_negotiated(&st, 4, (void *)0x4000), 0);

    if (g_fail) return 1;
    printf("uxfs lifetime state host ABI harness PASS\n");
    return 0;
}
