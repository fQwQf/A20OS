/*
 * Host differential harness for the block cache state engine
 * (BLOCK_CACHE_STATE_IMPL).
 *
 * Drives the C reference model (the pre-migration logic, reproduced below)
 * and the Rust engine (kernel/fs/block_cache_state.rs, linked as a host
 * staticlib) through one shared randomized op stream over two independent
 * worlds of pool entries, then compares every observable projection:
 * hash-bucket chains, LRU order, dirty counters, hold set, reference counts,
 * quarantine, sync selection and eviction verdicts.
 *
 * Exit 0 on success; on mismatch prints the op index and both projections and
 * exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- mirror structs (must match block_cache_state.rs and block_cache.h) ---- */
typedef struct BEntry {
    uint64_t lba;
    int dirty;
    uint64_t dirty_gen;
    int ref;
    int valid;
    unsigned char accessed;
    char data[512];
    struct BEntry *prev, *next, *hnext;
} BEntry;
typedef struct PEntry {
    uint64_t page_no;
    int dirty;
    uint64_t dirty_gen;
    int ref;
    int valid;
    unsigned char accessed;
    int held;
    char data[4096];
    struct PEntry *prev, *next, *hnext;
} PEntry;

#define BHASH 1024
#define PHASH 512
#define POOL 64

/* Rust ABI */
extern unsigned a20_bcache_hash_key(uint64_t);
extern unsigned a20_pcache_hash_key(uint64_t);
extern int a20_bcache_ref_read(const int *);
extern void a20_bcache_ref_get(int *);
extern void a20_bcache_ref_put(int *);
extern void a20_bcache_lru_insert_front(BEntry *, BEntry *);
extern void a20_bcache_lru_remove(BEntry *);
extern void a20_pcache_lru_insert_front(PEntry *, PEntry *);
extern void a20_pcache_lru_remove(PEntry *);
extern void a20_bcache_hash_insert_locked(BEntry **, BEntry *);
extern void a20_bcache_hash_remove_locked(BEntry **, BEntry *);
extern BEntry *a20_bcache_find_locked(BEntry **, uint64_t);
extern void a20_pcache_hash_insert_locked(PEntry **, PEntry *);
extern void a20_pcache_hash_remove_locked(PEntry **, PEntry *);
extern PEntry *a20_pcache_find_locked(PEntry **, uint64_t);
extern void a20_bcache_set_block_dirty(size_t *, BEntry *, int);
extern void a20_bcache_set_page_dirty(size_t *, PEntry *, int);
extern int a20_bcache_hold_page(PEntry *, size_t, uint64_t);
extern void a20_bcache_release_holds(PEntry *, size_t);
extern size_t a20_bcache_held_pages(const PEntry *, size_t);
extern int a20_bcache_write_quarantined(const uint64_t *, uint64_t);
extern uint64_t a20_bcache_note_write_error(uint64_t *, uint64_t, uint64_t);
extern int a20_bcache_sync_page_selected(const uint64_t *, size_t, uint64_t);
extern int a20_bcache_sync_select(int, int, int, int, const uint64_t *, size_t, uint64_t);
extern int a20_bcache_evict_decide(int, int, int, int, int, int);

/* ---- C reference logic (pre-migration copies) ---- */
static unsigned c_bhash(uint64_t lba) { return (unsigned)((lba ^ (lba >> 32)) & (BHASH - 1)); }
static unsigned c_phash(uint64_t pn) { return (unsigned)((pn ^ (pn >> 32)) & (PHASH - 1)); }
static void c_blru_rm(BEntry *e) { e->prev->next = e->next; e->next->prev = e->prev; }
static void c_blru_front(BEntry *h, BEntry *e) { e->next = h->next; e->prev = h; h->next->prev = e; h->next = e; }
static void c_plru_rm(PEntry *e) { e->prev->next = e->next; e->next->prev = e->prev; }
static void c_plru_front(PEntry *h, PEntry *e) { e->next = h->next; e->prev = h; h->next->prev = e; h->next = e; }
static void c_bins(BEntry **hash, BEntry *e) { unsigned i = c_bhash(e->lba); e->hnext = hash[i]; hash[i] = e; }
static void c_brms(BEntry **hash, BEntry *e) {
    unsigned i = c_bhash(e->lba); BEntry **pp = &hash[i];
    while (*pp) { if (*pp == e) { *pp = e->hnext; e->hnext = NULL; return; } pp = &(*pp)->hnext; }
    e->hnext = NULL;
}
static BEntry *c_bfind(BEntry **hash, uint64_t lba) {
    for (BEntry *e = hash[c_bhash(lba)]; e; e = e->hnext) if (e->valid && e->lba == lba) return e;
    return NULL;
}
static void c_pins(PEntry **hash, PEntry *e) { unsigned i = c_phash(e->page_no); e->hnext = hash[i]; hash[i] = e; }
static void c_prms(PEntry **hash, PEntry *e) {
    unsigned i = c_phash(e->page_no); PEntry **pp = &hash[i];
    while (*pp) { if (*pp == e) { *pp = e->hnext; e->hnext = NULL; return; } pp = &(*pp)->hnext; }
    e->hnext = NULL;
}
static PEntry *c_pfind(PEntry **hash, uint64_t pn) {
    for (PEntry *e = hash[c_phash(pn)]; e; e = e->hnext) if (e->valid && e->page_no == pn) return e;
    return NULL;
}
static void c_bdirty(size_t *n, BEntry *e, int d) {
    if (d) { e->dirty_gen++; if (!e->dirty) (*n)++; e->dirty = 1; }
    else if (e->dirty) { e->dirty = 0; if (*n > 0) (*n)--; }
}
static void c_pdirty(size_t *n, PEntry *e, int d) {
    if (d) { e->dirty_gen++; if (!e->dirty) (*n)++; e->dirty = 1; }
    else if (e->dirty) { e->dirty = 0; if (*n > 0) (*n)--; }
}
static int c_ref_read(const int *r) { return __atomic_load_n(r, __ATOMIC_ACQUIRE); }
static void c_ref_get(int *r) { __atomic_fetch_add(r, 1, __ATOMIC_ACQUIRE); }
static void c_ref_put(int *r) {
    int cur = __atomic_load_n(r, __ATOMIC_ACQUIRE);
    while (cur > 0) if (__atomic_compare_exchange_n(r, &cur, cur - 1, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) return;
}
static int c_quarantined(const uint64_t *u, uint64_t now) { return *u && now < *u; }
static uint64_t c_note_err(uint64_t *u, uint64_t now, uint64_t ticks) { *u = now + ticks; return *u; }
static int c_sync_sel(const uint64_t *nos, size_t n, uint64_t pn) {
    if (!nos) return 1;
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (nos[m] == pn) return 1; if (nos[m] < pn) lo = m + 1; else hi = m; }
    return 0;
}
static int c_sync_candidate(int mode, int valid, int dirty, int held, const uint64_t *nos, size_t n, uint64_t pn) {
    if (!valid || !dirty) return 0;
    if ((mode == 2) != (held != 0)) return 0;
    if (mode == 1 && !c_sync_sel(nos, n, pn)) return 0;
    return 1;
}
static int c_evict_decide(int q, int valid, int dirty, int acc, int held, int check_held) {
    if (!valid) return 1;
    if (check_held && held) return 0;
    if (q && dirty) return 0;
    if (acc) return 2;
    return 1;
}
static void c_hold(PEntry *pool, size_t n, uint64_t pn) {
    for (size_t i = 0; i < n; i++) if (pool[i].valid && pool[i].page_no == pn) { pool[i].held = 1; break; }
}
static void c_release_holds(PEntry *pool, size_t n) { for (size_t i = 0; i < n; i++) pool[i].held = 0; }
static size_t c_held_pages(const PEntry *pool, size_t n) {
    size_t k = 0; for (size_t i = 0; i < n; i++) if (pool[i].valid && pool[i].held) k++; return k;
}

/* ---- worlds ---- */
typedef struct {
    BEntry bpool[POOL];
    PEntry ppool[POOL];
    BEntry *bhash[BHASH];
    PEntry *phash[PHASH];
    BEntry blru_h, blru_t;
    PEntry plru_h, plru_t;
    size_t dirty_blocks, dirty_pages;
    uint64_t quar;
    int rust;
} world_t;

static void world_init(world_t *w)
{
    memset(w, 0, sizeof(*w));
    for (int i = 0; i < POOL; i++) {
        w->bpool[i].lba = (uint64_t)-1;
        w->ppool[i].page_no = (uint64_t)-1;
    }
    w->blru_h.next = &w->blru_t; w->blru_t.prev = &w->blru_h;
    w->plru_h.next = &w->plru_t; w->plru_t.prev = &w->plru_h;
    for (int i = 0; i < POOL; i++) {
        if (w->rust) { a20_bcache_lru_insert_front(&w->blru_h, &w->bpool[i]); a20_pcache_lru_insert_front(&w->plru_h, &w->ppool[i]); }
        else { c_blru_front(&w->blru_h, &w->bpool[i]); c_plru_front(&w->plru_h, &w->ppool[i]); }
    }
}

/* Canonical projection: LRU order as indices, hash chain heads, counters. */
static void project(const world_t *w, uint64_t *out, size_t *n)
{
    size_t k = 0;
    for (BEntry *e = w->blru_h.next; e != &w->blru_t; e = e->next) out[k++] = (uint64_t)(e - w->bpool);
    out[k++] = 0xFFFFFFFFu;
    for (PEntry *e = w->plru_h.next; e != &w->plru_t; e = e->next) out[k++] = (uint64_t)(e - w->ppool);
    out[k++] = 0xFFFFFFFFu;
    struct { BEntry *h; BEntry *e; } bs[POOL * 2]; int bn = 0;
    for (int i = 0; i < BHASH; i++)
        for (BEntry *e = w->bhash[i]; e; e = e->hnext) { bs[bn].h = e; bs[bn].e = e; bn++; }
    out[k++] = (uint64_t)bn;
    for (int i = 0; i < bn; i++) out[k++] = (uint64_t)(bs[i].e - w->bpool);
    struct { PEntry *e; } ps[POOL * 2]; int pn = 0;
    for (int i = 0; i < PHASH; i++)
        for (PEntry *e = w->phash[i]; e; e = e->hnext) { ps[pn++].e = e; }
    out[k++] = (uint64_t)pn;
    for (int i = 0; i < pn; i++) out[k++] = (uint64_t)(ps[i].e - w->ppool);
    out[k++] = w->dirty_blocks;
    out[k++] = w->dirty_pages;
    out[k++] = w->quar;
    *n = k;
}

static uint64_t rng_state = 0x123456789abcdef0ull;
static uint64_t rng_next(void) { uint64_t x = rng_state; x ^= x << 13; x ^= x >> 7; x ^= x << 17; rng_state = x; return x; }

#define OPS 40000
#define FAIL(...) do { fprintf(stderr, "block-cache-state MISMATCH (op %d): ", op); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int main(void)
{
    static world_t wc, wr;
    static uint64_t pc[2 * (POOL * 4 + 8)], pr[2 * (POOL * 4 + 8)];
    wc.rust = 0; wr.rust = 1;
    world_init(&wc); world_init(&wr);

    int op = 0;
    for (op = 0; op < OPS; op++) {
        uint64_t r = rng_next();
        unsigned kind = (unsigned)(r % 10u);
        unsigned i = (unsigned)((r >> 8) % POOL);
        uint64_t key = (r >> 16) & 0xFFFF;
        int *rc = &wc.bpool[i].ref, *rr = &wr.bpool[i].ref;
        switch (kind) {
        case 0: { /* block dirty */
            int d = (int)((r >> 5) & 1u);
            c_bdirty(&wc.dirty_blocks, &wc.bpool[i], d);
            a20_bcache_set_block_dirty(&wr.dirty_blocks, &wr.bpool[i], d);
            if (wc.bpool[i].dirty_gen != wr.bpool[i].dirty_gen) FAIL("b dirty_gen");
            break;
        }
        case 1: { /* page dirty */
            int d = (int)((r >> 5) & 1u);
            c_pdirty(&wc.dirty_pages, &wc.ppool[i], d);
            a20_bcache_set_page_dirty(&wr.dirty_pages, &wr.ppool[i], d);
            if (wc.ppool[i].dirty_gen != wr.ppool[i].dirty_gen) FAIL("p dirty_gen");
            break;
        }
        case 2: { /* block hash insert (valid) + find */
            c_brms(wc.bhash, &wc.bpool[i]);
            a20_bcache_hash_remove_locked(wr.bhash, &wr.bpool[i]);
            wc.bpool[i].lba = key; wc.bpool[i].valid = 1;
            wr.bpool[i].lba = key; wr.bpool[i].valid = 1;
            c_bins(wc.bhash, &wc.bpool[i]);
            a20_bcache_hash_insert_locked(wr.bhash, &wr.bpool[i]);
            BEntry *fc = c_bfind(wc.bhash, key);
            BEntry *fr = a20_bcache_find_locked(wr.bhash, key);
            if ((fc == NULL) != (fr == NULL)) FAIL("bfind null");
            if (fc && (fc - wc.bpool) != (fr - wr.bpool)) FAIL("bfind idx");
            break;
        }
        case 3: { /* block hash remove */
            c_brms(wc.bhash, &wc.bpool[i]);
            a20_bcache_hash_remove_locked(wr.bhash, &wr.bpool[i]);
            break;
        }
        case 4: { /* page hash insert + find */
            c_prms(wc.phash, &wc.ppool[i]);
            a20_pcache_hash_remove_locked(wr.phash, &wr.ppool[i]);
            wc.ppool[i].page_no = key; wc.ppool[i].valid = 1;
            wr.ppool[i].page_no = key; wr.ppool[i].valid = 1;
            c_pins(wc.phash, &wc.ppool[i]);
            a20_pcache_hash_insert_locked(wr.phash, &wr.ppool[i]);
            PEntry *fc = c_pfind(wc.phash, key);
            PEntry *fr = a20_pcache_find_locked(wr.phash, key);
            if ((fc == NULL) != (fr == NULL)) FAIL("pfind null");
            if (fc && (fc - wc.ppool) != (fr - wr.ppool)) FAIL("pfind idx");
            break;
        }
        case 5: { /* page hash remove */
            c_prms(wc.phash, &wc.ppool[i]);
            a20_pcache_hash_remove_locked(wr.phash, &wr.ppool[i]);
            break;
        }
        case 6: { /* lru move */
            c_blru_rm(&wc.bpool[i]); c_blru_front(&wc.blru_h, &wc.bpool[i]);
            a20_bcache_lru_remove(&wr.bpool[i]); a20_bcache_lru_insert_front(&wr.blru_h, &wr.bpool[i]);
            c_plru_rm(&wc.ppool[i]); c_plru_front(&wc.plru_h, &wc.ppool[i]);
            a20_pcache_lru_remove(&wr.ppool[i]); a20_pcache_lru_insert_front(&wr.plru_h, &wr.ppool[i]);
            break;
        }
        case 7: { /* ref get/put */
            if ((r >> 5) & 1u) { c_ref_get(rc); a20_bcache_ref_get(rr); }
            else { c_ref_put(rc); a20_bcache_ref_put(rr); }
            if (c_ref_read(rc) != a20_bcache_ref_read(rr)) FAIL("ref %d vs %d", c_ref_read(rc), a20_bcache_ref_read(rr));
            break;
        }
        case 8: { /* journal hold set */
            unsigned what = (unsigned)((r >> 5) % 3u);
            if (what == 0) {
                c_hold(wc.ppool, POOL, key);
                a20_bcache_hold_page(wr.ppool, POOL, key);
            } else if (what == 1) {
                c_release_holds(wc.ppool, POOL);
                a20_bcache_release_holds(wr.ppool, POOL);
            } else {
                if (c_held_pages(wc.ppool, POOL) != a20_bcache_held_pages(wr.ppool, POOL)) FAIL("held_pages");
            }
            for (int j = 0; j < POOL; j++)
                if (wc.ppool[j].held != wr.ppool[j].held) FAIL("held[%d]", j);
            break;
        }
        default: { /* quarantine */
            uint64_t now = r & 0xFFFF;
            int qc = c_quarantined(&wc.quar, now);
            int qr = a20_bcache_write_quarantined(&wr.quar, now);
            if (qc != qr) FAIL("quarantine");
            if ((r >> 5) & 1u) { uint64_t a = c_note_err(&wc.quar, now, 30); uint64_t b = a20_bcache_note_write_error(&wr.quar, now, 30); if (a != b) FAIL("note_err"); }
            break;
        }
        }

        /* Compare projections after every op (cheap enough at this POOL). */
        size_t nc = 0, nr = 0;
        project(&wc, pc, &nc);
        project(&wr, pr, &nr);
        if (nc != nr) FAIL("proj len %zu vs %zu", nc, nr);
        for (size_t j = 0; j < nc; j++)
            if (pc[j] != pr[j]) FAIL("proj[%zu] c=%llu rust=%llu", j, (unsigned long long)pc[j], (unsigned long long)pr[j]);
    }

    /* Policy differentials over the full input space. */
    op = -1;
    for (int q = 0; q < 2; q++) for (int v = 0; v < 2; v++) for (int d = 0; d < 2; d++)
        for (int a = 0; a < 2; a++) for (int h = 0; h < 2; h++) for (int ch = 0; ch < 2; ch++) {
            int x = c_evict_decide(q, v, d, a, h, ch);
            int y = a20_bcache_evict_decide(q, v, d, a, h, ch);
            if (x != y) FAIL("evict_decide(%d,%d,%d,%d,%d,%d) c=%d rust=%d", q, v, d, a, h, ch, x, y);
        }
    for (int mode = 0; mode < 3; mode++) for (int v = 0; v < 2; v++) for (int d = 0; d < 2; d++)
        for (int h = 0; h < 2; h++) {
            uint64_t nos[4] = {3, 9, 17, 1000};
            for (uint64_t pn = 0; pn < 40; pn++) {
                int x = c_sync_candidate(mode, v, d, h, nos, 4, pn);
                int y = a20_bcache_sync_select(mode, v, d, h, nos, 4, pn);
                if (x != y) FAIL("sync_select(%d,%d,%d,%d,%llu) c=%d rust=%d", mode, v, d, h, (unsigned long long)pn, x, y);
            }
        }
    if (a20_bcache_hash_key(0x100000003ull) != c_bhash(0x100000003ull)) FAIL("bhash");
    if (a20_pcache_hash_key(0x100000003ull) != c_phash(0x100000003ull)) FAIL("phash");
    puts("BLOCK_CACHE_STATE: PASS differential (40000 ops + policy space)");
    return 0;
}
