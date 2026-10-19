/*
 * Paired C/Rust microbenchmark for the canonical (sb, ino) vnode identity
 * index.  Both models run the identical deterministic op sequence against
 * their own fixed-size state, so a timing difference is algorithmic/ABI
 * overhead, not workload skew.
 *
 * Protocol (see tools/test-uxfs-vnode-identity-bench.sh):
 *   - a fixed number of rounds; each round runs both models and the round
 *     order alternates C->Rust / Rust->C so cache/thermal drift does not
 *     favour either model;
 *   - the driver reports the median per-model phase time (ns) over the rounds;
 *   - phases: fill (fresh insert), duplicate insert, lookup, remove, and a
 *     prune sweep of cache-only entries.
 *
 * The harness owns a logical vnode reference table shared by both models'
 * peek callbacks, mirroring the production adapter's non-blocking refcount
 * reader; a handle is cache-only (refcount 1) while the index owns it.
 */
#ifndef CONFIG_UXFS_VNODE_IDENTITY_RUST
#define CONFIG_UXFS_VNODE_IDENTITY_RUST 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs/vfs/vnode_identity.h"
#include "fs/vfs/vnode_identity_c.h"

#define CAP        16384u    /* identity table capacity (MAX_ENTRIES)      */
#define HANDLE_BASE (uintptr_t)0x10000
#define REFS_MAX   0x20000u
#define ROUNDS     31u
#define SB_BASE    0x70000000u

static uint8_t refs[REFS_MAX]; /* total refs per handle id                */

static uintptr_t peek_refs(uintptr_t vnode)
{
    if (vnode < HANDLE_BASE)
        return 0;
    uintptr_t id = vnode - HANDLE_BASE;
    return id < REFS_MAX ? refs[id] : 0;
}

typedef struct {
    const char *name;
    uintptr_t (*lookup)(uintptr_t sb, uint64_t ino);
    void (*insert)(uintptr_t sb, uint64_t ino, uintptr_t vnode,
                   a20_vnode_identity_insert_out_t *out);
    uintptr_t (*remove)(uintptr_t sb, uint64_t ino);
    size_t (*clear_sb)(uintptr_t sb, uintptr_t *out, size_t cap);
    size_t (*prune_batch)(uintptr_t *out, size_t cap);
    size_t (*count)(void);
    void (*set_ref_peek)(a20_vnode_identity_ref_peek_fn peek);
} vi_model_t;

static const vi_model_t model_c = {
    "c", a20_vnode_identity_lookup_c, a20_vnode_identity_insert_c,
    a20_vnode_identity_remove_c, a20_vnode_identity_clear_sb_c,
    a20_vnode_identity_prune_batch_c, a20_vnode_identity_count_c,
    a20_vnode_identity_set_ref_peek_c,
};
static const vi_model_t model_r = {
    "rust", a20_vnode_identity_lookup, a20_vnode_identity_insert,
    a20_vnode_identity_remove, a20_vnode_identity_clear_sb,
    a20_vnode_identity_prune_batch, a20_vnode_identity_count,
    a20_vnode_identity_set_ref_peek,
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uintptr_t handle_of(uint32_t key) { return HANDLE_BASE + key; }
static uintptr_t sb_of(uint32_t key) { return SB_BASE + (key & 7u); }
static uint64_t ino_of(uint32_t key) { return (uint64_t)key + 1u; }

/* Insert every key once, untimed; the index owns one reference per entry. */
static void prefill(const vi_model_t *m)
{
    for (uint32_t key = 0; key < CAP; key++) {
        a20_vnode_identity_insert_out_t out;
        m->insert(sb_of(key), ino_of(key), handle_of(key), &out);
        refs[key] = (out.status == A20_VNODE_IDENTITY_INSERTED) ? 1 : 0;
    }
}

static void reset_model(const vi_model_t *m)
{
    uintptr_t victims[256];
    for (int b = 0; b < 8; b++)
        while (m->clear_sb(SB_BASE + (uintptr_t)b, victims, 256))
            ;
    memset(refs, 0, sizeof(refs));
    m->set_ref_peek(peek_refs);
}

/* phase: 0 fill, 1 dup-insert, 2 lookup, 3 remove, 4 prune (cache-only). */
static uint64_t run_phase(const vi_model_t *m, int phase)
{
    uintptr_t victims[64];
    if (phase == 1 || phase == 4)
        prefill(m); /* setup is untimed for both models */
    uint64_t t0 = now_ns();
    switch (phase) {
    case 0:
        for (uint32_t key = 0; key < CAP; key++) {
            a20_vnode_identity_insert_out_t out;
            m->insert(sb_of(key), ino_of(key), handle_of(key), &out);
            refs[key] = (out.status == A20_VNODE_IDENTITY_INSERTED) ? 1 : 0;
        }
        break;
    case 1:
        for (uint32_t key = 0; key < CAP; key++) {
            a20_vnode_identity_insert_out_t out;
            m->insert(sb_of(key), ino_of(key), handle_of(key), &out);
        }
        break;
    case 2:
        for (uint32_t key = 0; key < CAP; key++)
            (void)m->lookup(sb_of(key), ino_of(key));
        break;
    case 3:
        for (uint32_t key = 0; key < CAP; key++) {
            uintptr_t got = m->remove(sb_of(key), ino_of(key));
            if (got >= HANDLE_BASE && got - HANDLE_BASE < REFS_MAX)
                refs[got - HANDLE_BASE] = 0;
        }
        break;
    default:
        for (;;) {
            size_t n = m->prune_batch(victims, 64);
            if (!n)
                break;
            for (size_t i = 0; i < n; i++)
                if (victims[i] >= HANDLE_BASE &&
                    victims[i] - HANDLE_BASE < REFS_MAX)
                    refs[victims[i] - HANDLE_BASE] = 0;
        }
        break;
    }
    return now_ns() - t0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    static uint64_t t_c[5][ROUNDS], t_r[5][ROUNDS];
    model_c.set_ref_peek(peek_refs);
    model_r.set_ref_peek(peek_refs);

    for (uint32_t round = 0; round < ROUNDS; round++) {
        int c_first = (round & 1u) == 0;
        const vi_model_t *first = c_first ? &model_c : &model_r;
        const vi_model_t *second = c_first ? &model_r : &model_c;
        uint64_t (*slot_first)[ROUNDS] = c_first ? t_c : t_r;
        uint64_t (*slot_second)[ROUNDS] = c_first ? t_r : t_c;
        for (int phase = 0; phase < 5; phase++) {
            reset_model(first);
            uint64_t a = run_phase(first, phase);
            reset_model(second);
            uint64_t b = run_phase(second, phase);
            slot_first[phase][round] = a;
            slot_second[phase][round] = b;
        }
    }

    static const char *names[5] = {"fill", "dup-insert", "lookup", "remove",
                                   "prune"};
    printf("phase,c_median_ns,rust_median_ns,rust_over_c\n");
    for (int phase = 0; phase < 5; phase++) {
        uint64_t cs[ROUNDS], rs[ROUNDS];
        memcpy(cs, t_c[phase], sizeof(cs));
        memcpy(rs, t_r[phase], sizeof(rs));
        qsort(cs, ROUNDS, sizeof(uint64_t), cmp_u64);
        qsort(rs, ROUNDS, sizeof(uint64_t), cmp_u64);
        uint64_t cm = cs[ROUNDS / 2], rm = rs[ROUNDS / 2];
        printf("%s,%lu,%lu,%.3f\n", names[phase], (unsigned long)cm,
               (unsigned long)rm, cm ? (double)rm / (double)cm : 0.0);
    }
    printf("ops_per_phase=%u rounds=%u\n", CAP, ROUNDS);
    return 0;
}
