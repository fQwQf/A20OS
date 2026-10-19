/*
 * Host differential harness for the canonical (sb, ino) vnode identity index.
 *
 * Drives the C reference model (vnode_identity_c.h) and the Rust
 * implementation (vnode_identity.rs, linked as a host staticlib through its
 * FFI) through the same operation sequences and compares every observable:
 * lookup results, insert statuses and `existing` handles, remove values,
 * clear/prune return counts, the exact evicted handle sequence, and live
 * entry counts.  Both models share one logical vnode reference table that
 * both peek callbacks consult, mirroring the production adapter's
 * non-blocking vnode reference reader.
 *
 * Scenarios, in order (each cleans up after itself):
 *   1. r4 identity: a second insert for a live (sb, ino) must report the
 *      canonical vnode as DUPLICATE -- the contract that makes two FDs of
 *      the same file share one vnode/page cache;
 *   2. prune must evict cache-only entries only, under a refcount oracle;
 *   3. rotating prune terminates across bounded batches;
 *   4. clear_sb chunked by capacity and does not disturb other superblocks;
 *   5. table exhaustion: FULL is reported, busy prunes yield nothing, and
 *      releasing references recovers the whole table;
 *   6. randomized op stream (fixed seed) with an oracle on every result.
 *
 * Exit status: 0 on success; 1 with a single stderr line naming the op index
 * and the diverging value on the first mismatch.
 */

#ifndef CONFIG_UXFS_VNODE_IDENTITY_RUST
#define CONFIG_UXFS_VNODE_IDENTITY_RUST 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs/vfs/vnode_identity.h"
#include "fs/vfs/vnode_identity_c.h"

#define TRACK_MAX   65536u  /* handle-id capacity for one harness process      */
#define HANDLE_BASE 0x10000u
#define KEY_POOL    4096u   /* registered test keys                            */
#define SB_A        0xA001u
#define SB_B        0xA002u
#define SB_C        0xA003u

#define MAX_ENTRIES 16384u
#define PRUNE_CAP   64u

static uint8_t  refs[TRACK_MAX];    /* logical total refs per handle id       */
static int32_t  key_of[TRACK_MAX];  /* pool key of a live handle, or -1       */
static uint64_t pool_ino[KEY_POOL];
static uint16_t pool_sb[KEY_POOL];
static uint8_t  present[KEY_POOL];  /* key has a live index entry             */
static uint32_t canon[KEY_POOL];    /* canonical handle id while live         */
static uint32_t next_id;
static uint64_t rng_state;
static size_t   op_count;

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
    "c",
    a20_vnode_identity_lookup_c,
    a20_vnode_identity_insert_c,
    a20_vnode_identity_remove_c,
    a20_vnode_identity_clear_sb_c,
    a20_vnode_identity_prune_batch_c,
    a20_vnode_identity_count_c,
    a20_vnode_identity_set_ref_peek_c,
};

static const vi_model_t model_r = {
    "rust",
    a20_vnode_identity_lookup,
    a20_vnode_identity_insert,
    a20_vnode_identity_remove,
    a20_vnode_identity_clear_sb,
    a20_vnode_identity_prune_batch,
    a20_vnode_identity_count,
    a20_vnode_identity_set_ref_peek,
};

#define FAIL(...)                                                       \
    do {                                                                \
        fprintf(stderr, "vnode-identity MISMATCH (op %zu): ", op_count);\
        fprintf(stderr, __VA_ARGS__);                                   \
        fputc('\n', stderr);                                            \
        return 1;                                                       \
    } while (0)

static uintptr_t mk_handle(uint32_t id)
{
    return (uintptr_t)HANDLE_BASE + id;
}

static uint32_t h_id(uintptr_t handle)
{
    return (uint32_t)(handle - HANDLE_BASE);
}

static uintptr_t peek_refs(uintptr_t vnode)
{
    if (vnode < HANDLE_BASE)
        return 0;
    {
        uint32_t id = h_id(vnode);
        if (id >= TRACK_MAX)
            return 0;
        return refs[id];
    }
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static int cmp_counts(void)
{
    size_t c = model_c.count();
    size_t r = model_r.count();
    if (c != r)
        FAIL("count c=%zu rust=%zu", c, r);
    return 0;
}

static void pool_config(uint32_t key, uint16_t sb, uint64_t ino)
{
    pool_sb[key] = sb;
    pool_ino[key] = ino;
    present[key] = 0;
    canon[key] = 0;
}

/* ---- operations, applied to both models in lockstep ------------------- */

static int op_insert(uint32_t key)
{
    uintptr_t cand = mk_handle(next_id++);
    a20_vnode_identity_insert_out_t oc, or;
    uint32_t id = h_id(cand);

    memset(&oc, 0xAA, sizeof(oc));
    memset(&or, 0xAA, sizeof(or));
    model_c.insert(pool_sb[key], pool_ino[key], cand, &oc);
    model_r.insert(pool_sb[key], pool_ino[key], cand, &or);
    op_count++;

    if (oc.status != or.status || oc.existing != or.existing)
        FAIL("insert key=%u status c=%d rust=%d existing c=%#lx rust=%#lx",
             key, oc.status, or.status, (unsigned long)oc.existing,
             (unsigned long)or.existing);

    switch (oc.status) {
    case A20_VNODE_IDENTITY_INSERTED:
        if (oc.existing != cand)
            FAIL("INSERTED existing %#lx != candidate %#lx",
                 (unsigned long)oc.existing, (unsigned long)cand);
        if (present[key])
            FAIL("INSERTED for already-present key=%u", key);
        present[key] = 1;
        canon[key] = id;
        key_of[id] = (int32_t)key;
        refs[id] = 1; /* the index owns one reference */
        break;
    case A20_VNODE_IDENTITY_DUPLICATE:
        if (!present[key])
            FAIL("DUPLICATE for absent key=%u", key);
        if (oc.existing != mk_handle(canon[key]))
            FAIL("DUPLICATE existing %#lx != canonical %#lx",
                 (unsigned long)oc.existing,
                 (unsigned long)mk_handle(canon[key]));
        refs[id] = 0;
        key_of[id] = -1;
        break;
    case A20_VNODE_IDENTITY_FULL:
        if (present[key])
            FAIL("FULL for present key=%u", key);
        refs[id] = 0;
        key_of[id] = -1;
        break;
    default:
        FAIL("insert key=%u unknown status %d", key, oc.status);
    }
    return cmp_counts();
}

static int op_lookup(uint32_t key)
{
    uintptr_t rc = model_c.lookup(pool_sb[key], pool_ino[key]);
    uintptr_t rr = model_r.lookup(pool_sb[key], pool_ino[key]);
    uintptr_t want = present[key] ? mk_handle(canon[key]) : 0;

    op_count++;
    if (rc != rr)
        FAIL("lookup key=%u c=%#lx rust=%#lx", key, (unsigned long)rc,
             (unsigned long)rr);
    if (rc != want)
        FAIL("lookup key=%u got %#lx want %#lx", key, (unsigned long)rc,
             (unsigned long)want);
    return 0;
}

static int op_remove(uint32_t key)
{
    uintptr_t rc = model_c.remove(pool_sb[key], pool_ino[key]);
    uintptr_t rr = model_r.remove(pool_sb[key], pool_ino[key]);

    op_count++;
    if (rc != rr)
        FAIL("remove key=%u c=%#lx rust=%#lx", key, (unsigned long)rc,
             (unsigned long)rr);

    if (present[key]) {
        if (rc != mk_handle(canon[key]))
            FAIL("remove key=%u got %#lx want canonical %#lx", key,
                 (unsigned long)rc, (unsigned long)mk_handle(canon[key]));
        present[key] = 0;
        refs[canon[key]] -= 1; /* caller puts the entry reference */
        key_of[canon[key]] = -1;
        canon[key] = 0;
    } else if (rc != 0) {
        FAIL("remove absent key=%u returned %#lx", key, (unsigned long)rc);
    }
    return cmp_counts();
}

static int op_fd_open(uint32_t key)
{
    op_count++;
    if (!present[key])
        FAIL("fd open for absent key=%u", key);
    if (refs[canon[key]] == 255)
        FAIL("refcount overflow key=%u", key);
    refs[canon[key]] += 1;
    return 0;
}

static int op_fd_close(uint32_t key)
{
    op_count++;
    if (!present[key])
        FAIL("fd close for absent key=%u", key);
    if (refs[canon[key]] <= 1)
        FAIL("fd close underflow key=%u", key);
    refs[canon[key]] -= 1;
    return 0;
}

static int consume_victims(const uintptr_t *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t id = h_id(buf[i]);
        int32_t key;

        if (buf[i] < HANDLE_BASE || id >= TRACK_MAX)
            FAIL("victim handle %#lx out of range", (unsigned long)buf[i]);
        if (refs[id] == 0)
            FAIL("victim handle %#lx had no reference", (unsigned long)buf[i]);
        key = key_of[id];
        if (key >= 0) {
            present[key] = 0;
            canon[key] = 0;
        }
        refs[id] -= 1; /* caller puts the evicted entry's reference */
        key_of[id] = -1;
    }
    return 0;
}

static int op_prune(size_t cap)
{
    uintptr_t bc[PRUNE_CAP], br[PRUNE_CAP];
    size_t nc, nr, i;

    if (cap == 0 || cap > PRUNE_CAP)
        FAIL("bad prune cap %zu", cap);
    nc = model_c.prune_batch(bc, cap);
    nr = model_r.prune_batch(br, cap);
    op_count++;
    if (nc != nr)
        FAIL("prune cap=%zu c=%zu rust=%zu", cap, nc, nr);
    for (i = 0; i < nc; i++)
        if (bc[i] != br[i])
            FAIL("prune entry %zu c=%#lx rust=%#lx", i, (unsigned long)bc[i],
                 (unsigned long)br[i]);
    if (consume_victims(bc, nc))
        return 1;
    return cmp_counts();
}

static int op_clear_sb(uint16_t sb, size_t cap)
{
    uintptr_t bc[PRUNE_CAP], br[PRUNE_CAP];
    size_t nc, nr, i;

    if (cap == 0 || cap > PRUNE_CAP)
        FAIL("bad clear cap %zu", cap);
    nc = model_c.clear_sb(sb, bc, cap);
    nr = model_r.clear_sb(sb, br, cap);
    op_count++;
    if (nc != nr)
        FAIL("clear_sb %#x cap=%zu c=%zu rust=%zu", (unsigned)sb, cap, nc,
             nr);
    for (i = 0; i < nc; i++)
        if (bc[i] != br[i])
            FAIL("clear_sb entry %zu c=%#lx rust=%#lx", i,
                 (unsigned long)bc[i], (unsigned long)br[i]);
    if (consume_victims(bc, nc))
        return 1;
    return cmp_counts();
}

/* Pick a random key that currently holds a live entry. */
static int pick_present(uint32_t *key)
{
    uint32_t start = (uint32_t)(rng_next() % KEY_POOL);
    uint32_t i;

    for (i = 0; i < KEY_POOL; i++) {
        uint32_t k = (start + i) % KEY_POOL;
        if (present[k]) {
            *key = k;
            return 1;
        }
    }
    return 0;
}

static uintptr_t sweep_bc[8192];
static uintptr_t sweep_br[8192];

/* One prune_batch step applied to both models, comparing the full victim
 * sequence before consuming it once into the shared bookkeeping. */
static int sweep_both(size_t cap, size_t *out_n)
{
    size_t nc, nr, i;

    if (cap == 0 || cap > 8192)
        FAIL("bad sweep cap %zu", cap);
    nc = model_c.prune_batch(sweep_bc, cap);
    nr = model_r.prune_batch(sweep_br, cap);
    op_count++;
    if (nc != nr)
        FAIL("sweep cap=%zu c=%zu rust=%zu", cap, nc, nr);
    for (i = 0; i < nc; i++)
        if (sweep_bc[i] != sweep_br[i])
            FAIL("sweep entry %zu c=%#lx rust=%#lx", i,
                 (unsigned long)sweep_bc[i], (unsigned long)sweep_br[i]);
    if (consume_victims(sweep_bc, nc))
        return 1;
    if (cmp_counts())
        return 1;
    *out_n = nc;
    return 0;
}

/* ---- scenarios -------------------------------------------------------- */

static int scenario_r4_identity(void)
{
    pool_config(0, SB_A, 42);
    pool_config(1, SB_B, 42);
    pool_config(2, SB_A, 7);

    /* Second open before the first write: DUPLICATE, same canonical vnode. */
    if (op_insert(0))
        return 1;
    if (op_insert(0))
        return 1;
    if (op_lookup(0))
        return 1;
    /* The same ino on another superblock is a different identity. */
    if (op_insert(1))
        return 1;
    if (op_insert(2))
        return 1;
    if (op_lookup(1))
        return 1;

    /* Both FDs hold live caller references. */
    if (op_fd_open(0))
        return 1;
    if (op_remove(1))
        return 1; /* unlink on B must not disturb A/0 */
    if (op_lookup(0))
        return 1;

    /* Unlink drops the identity: 0, and a re-create inserts a fresh vnode. */
    if (op_remove(0))
        return 1;
    if (op_lookup(0))
        return 1;
    if (op_insert(0))
        return 1;
    if (op_lookup(0))
        return 1;

    if (op_remove(0))
        return 1;
    if (op_remove(2))
        return 1;
    return cmp_counts();
}

static int scenario_prune_refcounts(void)
{
    pool_config(3, SB_A, 100);
    pool_config(4, SB_A, 101);
    pool_config(5, SB_A, 102);

    if (op_insert(3) || op_insert(4) || op_insert(5))
        return 1;
    if (op_fd_open(3))
        return 1; /* key 3 is busy (index + caller) */
    {
        uintptr_t bc[PRUNE_CAP], br[PRUNE_CAP];
        size_t nc, nr, i;

        nc = model_c.prune_batch(bc, PRUNE_CAP);
        nr = model_r.prune_batch(br, PRUNE_CAP);
        op_count++;
        if (nc != 2 || nr != 2)
            FAIL("busy prune evicted c=%zu rust=%zu want 2", nc, nr);
        for (i = 0; i < nc && i < nr; i++)
            if (bc[i] != br[i])
                FAIL("busy prune order differs at %zu", i);
        if (consume_victims(bc, nc))
            return 1;
    }
    if (op_lookup(3))
        return 1;
    if (op_lookup(4) || op_lookup(5))
        return 1;
    if (op_fd_close(3))
        return 1;
    if (op_prune(PRUNE_CAP))
        return 1;
    if (op_lookup(3))
        return 1;
    return cmp_counts();
}

static int scenario_cursor_termination(void)
{
    int guard;

    pool_config(6, SB_A, 200);
    pool_config(7, SB_A, 201);
    pool_config(8, SB_A, 202);
    if (op_insert(6) || op_insert(7) || op_insert(8))
        return 1;
    for (guard = 0; guard < 8; guard++) {
        uintptr_t bc[PRUNE_CAP], br[PRUNE_CAP];
        size_t nc = model_c.prune_batch(bc, 1);
        size_t nr = model_r.prune_batch(br, 1);

        op_count++;
        if (nc != nr || (nc && bc[0] != br[0]))
            FAIL("cap=1 prune step %d c=%zu rust=%zu", guard, nc, nr);
        if (consume_victims(bc, nc))
            return 1;
        if (nc == 0)
            break;
    }
    if (guard == 8)
        FAIL("cap=1 prune did not terminate");
    return cmp_counts();
}

static int scenario_clear_sb_chunked(void)
{
    pool_config(9, SB_A, 300);
    pool_config(10, SB_A, 301);
    pool_config(11, SB_A, 302);
    pool_config(12, SB_B, 300);
    pool_config(13, SB_B, 301);
    if (op_insert(9) || op_insert(10) || op_insert(11) || op_insert(12) ||
        op_insert(13))
        return 1;
    if (op_clear_sb(SB_A, 2))
        return 1;
    if (op_clear_sb(SB_A, 2))
        return 1;
    if (op_clear_sb(SB_A, 2))
        return 1;
    if (op_lookup(9) || op_lookup(10) || op_lookup(11))
        return 1;
    if (op_lookup(12) || op_lookup(13))
        return 1;
    if (op_remove(12) || op_remove(13))
        return 1;
    return cmp_counts();
}

static int scenario_exhaustion(void)
{
    const uint16_t sb = 0xE000u;
    uint32_t first_id = next_id;
    uint32_t i;
    size_t n;

    /* Fill the whole table with busy entries (index + caller). */
    for (i = 0; i < MAX_ENTRIES; i++) {
        uintptr_t cand = mk_handle(next_id++);
        a20_vnode_identity_insert_out_t oc, or;

        memset(&oc, 0xAA, sizeof(oc));
        memset(&or, 0xAA, sizeof(or));
        model_c.insert(sb, i, cand, &oc);
        model_r.insert(sb, i, cand, &or);
        op_count++;
        if (oc.status != A20_VNODE_IDENTITY_INSERTED ||
            or.status != A20_VNODE_IDENTITY_INSERTED)
            FAIL("fill step %u c=%d rust=%d", i, oc.status, or.status);
        refs[h_id(cand)] = 2;
    }
    if (cmp_counts())
        return 1;
    if (model_c.count() != MAX_ENTRIES)
        FAIL("fill count c=%zu want %u", model_c.count(), MAX_ENTRIES);

    /* FULL: nothing inserted, nothing changed. */
    {
        uintptr_t cand = mk_handle(next_id++);
        a20_vnode_identity_insert_out_t oc, or;

        memset(&oc, 0xAA, sizeof(oc));
        memset(&or, 0xAA, sizeof(or));
        model_c.insert(sb, MAX_ENTRIES, cand, &oc);
        model_r.insert(sb, MAX_ENTRIES, cand, &or);
        op_count++;
        if (oc.status != A20_VNODE_IDENTITY_FULL ||
            or.status != A20_VNODE_IDENTITY_FULL)
            FAIL("full probe c=%d rust=%d", oc.status, or.status);
        if (oc.existing != 0 || or.existing != 0)
            FAIL("full probe existing c=%#lx rust=%#lx",
                 (unsigned long)oc.existing, (unsigned long)or.existing);
    }

    /* Spot-check lookups across the table. */
    for (i = 0; i < 64; i++) {
        uint64_t ino = rng_next() % MAX_ENTRIES;
        uintptr_t rc = model_c.lookup(sb, ino);
        uintptr_t rr = model_r.lookup(sb, ino);

        op_count++;
        if (rc != rr)
            FAIL("exhaustion lookup ino=%lu c=%#lx rust=%#lx",
                 (unsigned long)ino, (unsigned long)rc, (unsigned long)rr);
        if (rc != mk_handle(first_id + (uint32_t)ino))
            FAIL("exhaustion lookup ino=%lu got %#lx", (unsigned long)ino,
                 (unsigned long)rc);
    }

    /* Busy prune: nothing evictable, over bounded batches. */
    if (op_prune(PRUNE_CAP))
        return 1;
    if (op_prune(PRUNE_CAP))
        return 1;

    /* Release the caller references; the rotating cursor sweeps everything
     * over bounded batches in lockstep. */
    for (i = 0; i < MAX_ENTRIES; i++)
        refs[first_id + i] = 1;

    if (sweep_both(8192, &n))
        return 1;
    if (n != 8192)
        FAIL("first sweep c=%zu want 8192", n);
    if (sweep_both(8192, &n))
        return 1;
    if (n != MAX_ENTRIES - 8192)
        FAIL("second sweep c=%zu want %u", n, MAX_ENTRIES - 8192);
    if (sweep_both(8192, &n))
        return 1;
    if (n != 0)
        FAIL("third sweep c=%zu want 0", n);
    if (model_c.count() != 0)
        FAIL("exhaustion table not empty");

    /* Capacity recovered for new keys. */
    {
        uintptr_t cand = mk_handle(next_id++);
        a20_vnode_identity_insert_out_t oc, or;

        memset(&oc, 0xAA, sizeof(oc));
        memset(&or, 0xAA, sizeof(or));
        model_c.insert(sb, 7, cand, &oc);
        model_r.insert(sb, 7, cand, &or);
        op_count++;
        if (oc.status != A20_VNODE_IDENTITY_INSERTED ||
            or.status != A20_VNODE_IDENTITY_INSERTED)
            FAIL("post-sweep insert c=%d rust=%d", oc.status, or.status);
        refs[h_id(cand)] = 1;
    }
    if (op_prune(PRUNE_CAP))
        return 1;
    return cmp_counts();
}

/* Re-key every pool slot for the randomized phase: unique (sb, ino) per slot
 * so an insert of slot k can never alias another slot's key. */
static void pool_setup_random(void)
{
    uint32_t k;

    for (k = 0; k < KEY_POOL; k++) {
        pool_sb[k] = (uint16_t)(SB_A + (k % 3));
        pool_ino[k] = 0x1000u + k;
        present[k] = 0;
        canon[k] = 0;
    }
}

static int run_random_phase(unsigned ops, uint64_t seed)
{
    unsigned i;

    pool_setup_random();
    rng_state = seed ? seed : 1;
    for (i = 0; i < ops; i++) {
        uint64_t roll = rng_next() % 100;
        uint32_t key = (uint32_t)(rng_next() % KEY_POOL);

        if (roll < 30) {
            if (op_insert(key))
                return 1;
        } else if (roll < 45) {
            if (op_insert(key) || op_insert(key)) /* dup probe */
                return 1;
        } else if (roll < 65) {
            if (op_lookup(key))
                return 1;
            if ((i & 7u) == 0) { /* periodic sweep over a few keys */
                uint32_t j;
                for (j = 0; j < 4; j++)
                    if (op_lookup((uint32_t)(rng_next() % KEY_POOL)))
                        return 1;
            }
        } else if (roll < 80) {
            if (op_remove(key))
                return 1;
        } else if (roll < 88) {
            uint32_t k;
            if (pick_present(&k) && op_fd_open(k))
                return 1;
        } else if (roll < 94) {
            uint32_t k, tries;
            for (tries = 0; tries < 8; tries++) {
                k = (uint32_t)(rng_next() % KEY_POOL);
                if (present[k] && refs[canon[k]] > 1) {
                    if (op_fd_close(k))
                        return 1;
                    break;
                }
            }
        } else if (roll < 97) {
            if (op_prune(1 + (size_t)(rng_next() % 8)))
                return 1;
        } else {
            static const uint16_t sbs[3] = { SB_A, SB_B, SB_C };
            uint16_t sb = sbs[rng_next() % 3];
            if (op_clear_sb(sb, 1 + (size_t)(rng_next() % 8)))
                return 1;
        }
    }

    /* Final full comparison, then remove everything so the phase is exact. */
    {
        uint32_t key;
        for (key = 0; key < KEY_POOL; key++)
            if (op_lookup(key))
                return 1;
        for (key = 0; key < KEY_POOL; key++)
            if (present[key] && op_remove(key))
                return 1;
    }
    return cmp_counts();
}

int main(void)
{
    memset(key_of, 0xFF, sizeof(key_of)); /* -1 */
    model_c.set_ref_peek(peek_refs);
    model_r.set_ref_peek(peek_refs);

    if (scenario_r4_identity())
        return 1;
    if (scenario_prune_refcounts())
        return 1;
    if (scenario_cursor_termination())
        return 1;
    if (scenario_clear_sb_chunked())
        return 1;
    if (scenario_exhaustion())
        return 1;
    if (run_random_phase(40000, 0x5EED20261015ull))
        return 1;

    /* Every op above ends with both models empty. */
    if (cmp_counts())
        return 1;
    if (model_c.count() != 0)
        FAIL("table not empty at end");

    printf("vnode identity C/Rust differential PASS (ops=%zu)\n", op_count);
    return 0;
}
