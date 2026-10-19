/*
 * Host differential harness for kernel/mm/madvise_policy.rs.
 *
 * The production Rust is linked against a scripted model of the address space:
 * a list of mapping records, a leaf per address, and counters for every
 * primitive the transaction calls.  The assertions are about the decisions
 * Rust owns -- which advice acts and which is refused, the range and coverage
 * contract, the discard transaction's owner classification and its
 * exactly-once release, the fork-policy transaction's validate-then-split-then-
 * mark order, and the VMO export qualification.
 *
 * Guest coverage is separate: mm_stress/mmprobe drive MADV_DONTNEED and the
 * fork-policy advice through the real page tables.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define USER_VA_LIMIT ((uintptr_t)1 << 38)

#define VM_DONTFORK   (1ULL << 9)
#define VM_WIPEONFORK (1ULL << 10)
#define VM_VMO        (1ULL << 14)
#define VM_LOCKED     (1ULL << 15)
#define VM_PFNMAP     (1ULL << 17)
#define VM_NATIVE_LOCKED (1ULL << 27)

static int failures, checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- model -- */

struct a20_madvise_vma {
    void *vma;
    uintptr_t start, end;
    uint64_t vm_flags;
};

struct a20_madvise_leaf {
    int32_t present, level;
    uintptr_t base, size;
    uint32_t pfn, pad;
};

struct a20_madvise_unmapped {
    int32_t ok;
    uintptr_t base, size, pa;
};

#define MAX_RECS 24
#define MAX_LEAVES 12

struct rec { void *ptr; void *vmo; uintptr_t start, end; uint64_t flags; };
struct leaf { uintptr_t va; int present, level; uintptr_t base, size; uint32_t pfn;
              void *pcp; int hold_rc, unmap_rc; uintptr_t pa; };

static struct rec g_recs[MAX_RECS];
static int g_nrecs;
static struct leaf g_leaves[MAX_LEAVES];
static int g_nleaves;

static int g_locked, lock_calls, unlock_calls;
static int collect_calls, vma_at_calls, split_calls, set_flags_calls;
static int probe_calls, hold_calls, demote_calls, unmap_calls;
static int note_calls, release_calls, drop_calls, rss_calls;
static int frame_puts, cache_puts;
static int released_kind[MAX_LEAVES];
static int g_released, g_pages_total;
static uint64_t g_set_flags_last, g_set_on_last;
static void *g_set_vma_last;
static int g_split_at_start, g_split_at_end;
static int g_demote_reply;
static int g_hold_fail_va_set;
static uintptr_t g_hold_fail_va;
static int g_cache_puts_while_locked, g_charge_locked;
static int pageout_calls, pageout_reply, pageout_len;
static uintptr_t pageout_addr;
static void *g_current_mm, *g_mm_token;
static int ref_calls;
static uint32_t g_prot_reply;

static void model_reset(void)
{
    memset(g_recs, 0, sizeof(g_recs));
    memset(g_leaves, 0, sizeof(g_leaves));
    g_nrecs = g_nleaves = 0;
    g_locked = lock_calls = unlock_calls = 0;
    collect_calls = vma_at_calls = split_calls = set_flags_calls = 0;
    probe_calls = hold_calls = demote_calls = unmap_calls = 0;
    note_calls = release_calls = drop_calls = rss_calls = 0;
    frame_puts = cache_puts = 0;
    memset(released_kind, 0, sizeof(released_kind));
    g_released = g_pages_total = 0;
    g_set_flags_last = 0; g_set_on_last = -1; g_set_vma_last = NULL;
    g_split_at_start = g_split_at_end = 0;
    g_demote_reply = 0;
    g_hold_fail_va_set = 0; g_hold_fail_va = 0;
    g_cache_puts_while_locked = g_charge_locked = 0;
    pageout_calls = pageout_reply = pageout_len = 0;
    pageout_addr = 0;
    g_mm_token = (void *)(uintptr_t)0x3A3A;
    g_current_mm = g_mm_token;
    ref_calls = 0;
    g_prot_reply = 0;
}

static void add_rec(uintptr_t start, uintptr_t end, uint64_t flags)
{
    struct rec *r = &g_recs[g_nrecs++];
    r->ptr = (void *)(uintptr_t)(0x1000 + g_nrecs);
    r->vmo = (void *)(uintptr_t)0x7B7B;
    r->start = start; r->end = end; r->flags = flags;
}

static struct leaf *add_leaf(uintptr_t va, int level, uintptr_t base, uintptr_t size)
{
    struct leaf *l = &g_leaves[g_nleaves++];
    l->va = va; l->present = 1; l->level = level;
    l->base = base; l->size = size; l->pfn = (uint32_t)(va / PAGE_SIZE);
    l->pcp = NULL; l->hold_rc = 0; l->unmap_rc = 0; l->pa = va + 0x80000000ULL;
    return l;
}

/* --------------------------------------------------------------- bridges - */

uintptr_t a20_madvise_user_va_limit(void) { return USER_VA_LIMIT; }
int a20_madvise_is_nommu(void) { return 0; }

uint64_t a20_madvise_lock(void *mm)
{
    check(mm == g_mm_token, "lock: called with the caller's mm");
    lock_calls++; g_locked = 1;
    return 0xA5A5;
}

void a20_madvise_unlock(void *mm, uint64_t flags)
{
    (void)mm; (void)flags;
    unlock_calls++; g_locked = 0;
}

int a20_madvise_collect(void *mm, uintptr_t addr, uintptr_t end,
                        struct a20_madvise_vma *out, uintptr_t cap)
{
    (void)mm;
    collect_calls++;
    uintptr_t n = 0;
    for (int i = 0; i < g_nrecs; i++) {
        if (g_recs[i].start >= end) break;
        if (g_recs[i].end <= addr) continue;
        if (n == cap) return -28; /* -ENOSPC */
        out[n].vma = g_recs[i].ptr;
        out[n].start = g_recs[i].start;
        out[n].end = g_recs[i].end;
        out[n].vm_flags = g_recs[i].flags;
        n++;
    }
    return (int)n;
}

int a20_madvise_vma_at(void *mm, uintptr_t va, struct a20_madvise_vma *out)
{
    (void)mm;
    vma_at_calls++;
    for (int i = 0; i < g_nrecs; i++) {
        if (va >= g_recs[i].start && va < g_recs[i].end) {
            out->vma = g_recs[i].ptr;
            out->start = g_recs[i].start;
            out->end = g_recs[i].end;
            out->vm_flags = g_recs[i].flags;
            return 0;
        }
    }
    return -12;
}

void a20_madvise_vma_set_flags(void *vma, uint64_t flags, int on)
{
    set_flags_calls++;
    g_set_vma_last = vma; g_set_flags_last = flags; g_set_on_last = on;
}

uint64_t a20_madvise_vma_flags(void *vma)
{
    for (int i = 0; i < g_nrecs; i++)
        if (g_recs[i].ptr == vma) return g_recs[i].flags;
    return 0;
}

uintptr_t a20_madvise_vma_pte_flags(void *vma) { (void)vma; return 0x123; }

void *a20_madvise_vmo_covering(void *mm, uintptr_t addr, uintptr_t end)
{
    (void)mm;
    for (int i = 0; i < g_nrecs; i++)
        if (g_recs[i].start <= addr && g_recs[i].end >= end) return g_recs[i].ptr;
    return NULL;
}

void *a20_madvise_vmo_ref(void *vma)
{
    ref_calls++;
    for (int i = 0; i < g_nrecs; i++)
        if (g_recs[i].ptr == vma) return g_recs[i].vmo;
    return NULL;
}

int mm_pte_flags_to_prot(uintptr_t pte_flags)
{
    (void)pte_flags;
    return (int)g_prot_reply;
}

void *a20_madvise_current_mm(void) { return g_current_mm; }

int a20_madvise_pageout_range(void *mm, uintptr_t addr, uintptr_t end)
{
    (void)mm;
    pageout_calls++; pageout_addr = addr; pageout_len = (int)(end - addr);
    return pageout_reply;
}

int a20_madvise_split(void *mm, uintptr_t va)
{
    (void)mm;
    split_calls++;
    if (split_calls == 1) g_split_at_start = (int)(va / PAGE_SIZE);
    else if (split_calls == 2) g_split_at_end = (int)(va / PAGE_SIZE);
    return 0;
}

void a20_madvise_leaf_probe(void *mm, uintptr_t va, struct a20_madvise_leaf *out)
{
    (void)mm;
    probe_calls++;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < g_nleaves; i++) {
        if (g_leaves[i].va == va) {
            out->present = g_leaves[i].present;
            out->level = g_leaves[i].level;
            out->base = g_leaves[i].base;
            out->size = g_leaves[i].size;
            out->pfn = g_leaves[i].pfn;
            return;
        }
    }
    out->present = 0;
}

int a20_madvise_leaf_hold(void *mm, void *vma, uintptr_t va, uint32_t pfn,
                          int no_frame_ref, void **out_pcp)
{
    (void)mm; (void)vma; (void)pfn; (void)no_frame_ref;
    hold_calls++;
    *out_pcp = NULL;
    if (g_hold_fail_va_set && g_hold_fail_va == va)
        return -12;
    for (int i = 0; i < g_nleaves; i++) {
        if (g_leaves[i].va == va) {
            *out_pcp = g_leaves[i].pcp;
            return g_leaves[i].hold_rc;
        }
    }
    return 0;
}

int a20_madvise_demote(void *mm, uintptr_t va)
{
    (void)mm;
    demote_calls++;
    if (g_demote_reply < 0)
        return g_demote_reply;
    /* The split turns the straddling huge leaf into a base-page leaf so the
     * transaction can make progress on the next probe. */
    for (int i = 0; i < g_nleaves; i++) {
        if (g_leaves[i].va == va) {
            g_leaves[i].level = 0;
            g_leaves[i].base = va;
            g_leaves[i].size = PAGE_SIZE;
        }
    }
    return 0;
}

void a20_madvise_leaf_unmap(void *mm, uintptr_t va, struct a20_madvise_unmapped *out)
{
    (void)mm;
    unmap_calls++;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < g_nleaves; i++) {
        if (g_leaves[i].va == va) {
            if (g_leaves[i].unmap_rc != 0) {
                out->ok = g_leaves[i].unmap_rc;
                return;
            }
            out->ok = 0;
            out->base = g_leaves[i].base;
            out->size = g_leaves[i].size;
            out->pa = g_leaves[i].pa;
            return;
        }
    }
    out->ok = -1;
}

void a20_madvise_note_change(void *mm, uintptr_t base, uintptr_t size)
{
    (void)mm; (void)base; (void)size;
    note_calls++;
}

void a20_madvise_drop_cache_ref(void *pcp)
{
    if (pcp) cache_puts++;
    drop_calls++;
    if (g_locked) g_cache_puts_while_locked++;
}

void a20_madvise_release(void *mm, int kind, void *pcp, uintptr_t pa, uintptr_t pages)
{
    (void)mm;
    release_calls++;
    if (kind == 0) { g_released++; g_pages_total += (int)pages; return; }
    if (!pa) return;
    if (kind == 2 && pcp) { cache_puts += 2; }
    else if (kind == 1) { frame_puts++; }
    released_kind[g_released] = kind;
    g_released++;
    g_pages_total += (int)pages;
}

void mm_tlb_invalidate_begin(void *mm) { (void)mm; }
void mm_tlb_invalidate_finish(void *mm) { (void)mm; }

extern int a20_madvise_dontneed(void *mm, uintptr_t addr, uintptr_t len);
extern int a20_madvise_fork_flags(void *mm, uintptr_t start, uintptr_t len,
                                  uint64_t flags, int on);
extern int a20_madvise_dispatch(void *mm, uintptr_t addr, uintptr_t len, int advice);
extern int a20_madvise_set_lock(void *mm, uintptr_t start, uintptr_t end, int on);
extern void *a20_madvise_vmo_region(void *mm, uintptr_t addr, uintptr_t len,
                                    uint32_t *prot_out);

/* ------------------------------------------------------------- scenarios - */

static void range_contract(void)
{
    model_reset();
    add_rec(0x10000, 0x20000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);

    check(a20_madvise_dispatch(g_mm_token, 0x10001, PAGE_SIZE, 4) == -22,
          "range: an unaligned address is EINVAL");
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0, 4) == -22,
          "range: a zero length is EINVAL");
    check(a20_madvise_dispatch(NULL, 0x10000, PAGE_SIZE, 4) == -22,
          "range: a null mm is EINVAL");
    check(a20_madvise_dispatch(g_mm_token, USER_VA_LIMIT, PAGE_SIZE, 4) == -22,
          "range: an address at the limit is EINVAL");
    check(a20_madvise_dispatch(g_mm_token, USER_VA_LIMIT - PAGE_SIZE, 2 * PAGE_SIZE, 4) == -22,
          "range: a length past the limit is EINVAL");
    check(a20_madvise_dispatch(g_mm_token, USER_VA_LIMIT - 8, 16, 4) == -22,
          "range: a length that wraps the limit is EINVAL");
    check(lock_calls == 0, "range: a rejected range takes no lock");

    /* A length that is not page-aligned is rounded up before coverage, so a
     * record that ends inside the last requested page is not enough. */
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE + 1, 4) == -12,
          "range: coverage is checked against the rounded end");
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 1, 4) == 0,
          "range: a short request inside the record is accepted");
}

static void coverage(void)
{
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_rec(0x11000, 0x12000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 4) == 0,
          "coverage: two tiled records cover the range");
    check(collect_calls == 1, "coverage: the walk is batched, not per page");
    check(vma_at_calls == 0, "coverage: no per-record probe when the batch fits");

    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_rec(0x12000, 0x13000, 0); /* hole at 0x11000 */
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x3000, 4) == -12,
          "coverage: a hole in the range is ENOMEM");
    check(unmap_calls == 0 && release_calls == 0,
          "coverage: nothing is discarded when the range has a hole");

    model_reset();
    add_rec(0x11000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 4) == -12,
          "coverage: a range that starts before the first record is ENOMEM");

    model_reset();
    add_rec(0x10000, 0x11000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 4) == -12,
          "coverage: a range that ends past the last record is ENOMEM");

    /* More records than the batch buffer holds: the fallback probe path. */
    model_reset();
    for (int i = 0; i < 20; i++)
        add_rec(0x10000 + (uintptr_t)i * PAGE_SIZE, 0x10000 + (uintptr_t)(i + 1) * PAGE_SIZE, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 20 * PAGE_SIZE, 4) == 0,
          "coverage: a range wider than the batch buffer is still covered");
    check(vma_at_calls >= 20, "coverage: the overflow falls back to per-record probes");
}

static void advice_table(void)
{
    /* Every advice the table classifies, observed through what it does. */
    model_reset();
    add_rec(0x10000, 0x20000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE, 9) == -38,
          "table: MADV_REMOVE is ENOSYS");
    check(lock_calls == 0, "table: MADV_REMOVE touches nothing");

    check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE, 99) == -22,
          "table: an unrecognised advice is EINVAL");
    check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE, -1) == -22,
          "table: a negative advice is EINVAL");

    /* The ignored advices still validate coverage. */
    static const int ignored[] = {0, 1, 2, 3, 12, 13, 14, 15, 16, 17, 20, 22, 23};
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        model_reset();
        add_rec(0x10000, 0x11000, 0);
        check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE, ignored[i]) == 0,
              "table: an ignored advice succeeds on a covered range");
        check(unmap_calls == 0 && set_flags_calls == 0 && split_calls == 0,
              "table: an ignored advice mutates nothing");
        model_reset();
        add_rec(0x12000, 0x13000, 0);
        check(a20_madvise_dispatch(g_mm_token, 0x10000, PAGE_SIZE, ignored[i]) == -12,
              "table: an ignored advice still refuses an uncovered range");
    }

    /* The fork advices. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 10) == 0,
          "table: MADV_DONTFORK succeeds");
    check(g_set_flags_last == VM_DONTFORK && g_set_on_last == 1,
          "table: MADV_DONTFORK sets the DONTFORK bit");

    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 11) == 0,
          "table: MADV_DOFORK succeeds");
    check(g_set_flags_last == VM_DONTFORK && g_set_on_last == 0,
          "table: MADV_DOFORK clears the DONTFORK bit");

    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 18) == 0,
          "table: MADV_WIPEONFORK succeeds");
    check(g_set_flags_last == (VM_DONTFORK | VM_WIPEONFORK) && g_set_on_last == 1,
          "table: MADV_WIPEONFORK sets DONTFORK too");

    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 19) == 0,
          "table: MADV_KEEPONFORK succeeds");
    check(g_set_flags_last == VM_WIPEONFORK && g_set_on_last == 0,
          "table: MADV_KEEPONFORK clears only WIPEONFORK");
}

static void fork_flags_transaction(void)
{
    /* PFNMAP is refused before anything moves. */
    model_reset();
    add_rec(0x10000, 0x12000, VM_PFNMAP);
    check(a20_madvise_fork_flags(g_mm_token, 0x10000, 0x2000, VM_DONTFORK, 1) == -1,
          "fork: a VM_PFNMAP range is EPERM");
    check(split_calls == 0 && set_flags_calls == 0,
          "fork: a refused range is neither split nor marked");

    /* A hole is refused before anything moves. */
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_rec(0x12000, 0x13000, 0);
    check(a20_madvise_fork_flags(g_mm_token, 0x10000, 0x3000, VM_DONTFORK, 1) == -12,
          "fork: a hole is ENOMEM");
    check(split_calls == 0 && set_flags_calls == 0,
          "fork: an uncovered range is neither split nor marked");

    /* Both boundaries are split before any bit moves. */
    model_reset();
    add_rec(0x10000, 0x14000, 0);
    check(a20_madvise_fork_flags(g_mm_token, 0x11000, 0x2000, VM_DONTFORK, 1) == 0,
          "fork: a covered sub-range succeeds");
    check(split_calls == 2, "fork: both boundaries are split");
    check(g_split_at_start == 0x11 && g_split_at_end == 0x13,
          "fork: the splits are at the requested boundaries");
    check(set_flags_calls == 1, "fork: the range is marked once after the split");
    check(lock_calls == 1 && unlock_calls == 1, "fork: one locked region");
}

static void discard_owners(void)
{
    /* An anonymous frame: one frame_put, one rss subtraction. */
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: an anonymous frame is discarded");
    check(frame_puts == 1 && cache_puts == 0, "discard: the frame is put exactly once");
    check(released_kind[0] == 1, "discard: the owner is classified as a frame");
    check(note_calls == 1 && g_pages_total == 1, "discard: one page is noted and unaccounted");

    /* A page-cache leaf: the lookup ref plus the mapping's pin. */
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    struct leaf *l = add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    l->pcp = (void *)(uintptr_t)0xCACE;
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: a cache leaf is discarded");
    check(cache_puts == 2 && frame_puts == 0,
          "discard: a cache leaf releases the lookup ref and the mapping pin");
    check(released_kind[0] == 2, "discard: the owner is classified as a cache page");

    /* A VMO record: the PTE goes, the frame is not put at all. */
    model_reset();
    add_rec(0x10000, 0x11000, VM_VMO);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: a VMO leaf is unmapped");
    check(hold_calls == 0, "discard: a VMO leaf takes no frame reference");
    check(frame_puts == 0 && cache_puts == 0, "discard: a VMO frame is not released");
    check(g_pages_total == 1, "discard: the VMO page is still unaccounted");

    /* A PFNMAP record behaves the same way. */
    model_reset();
    add_rec(0x10000, 0x11000, VM_PFNMAP);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: a PFNMAP leaf is unmapped");
    check(hold_calls == 0 && frame_puts == 0,
          "discard: a PFNMAP frame is not released");

    /* A huge leaf that straddles the request end is split first. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    add_leaf(0x10000, 2, 0x10000, 2 * PAGE_SIZE); /* 2 MiB leaf over a 4 KiB request */
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: a straddling huge leaf is handled");
    check(demote_calls == 1, "discard: the straddling huge leaf is split");
    check(frame_puts == 1, "discard: only the requested page is released");
    check(unmap_calls == 1 && probe_calls == 2,
          "discard: the leaf is unmapped once, after the split turned it into a base page");

    /* A leaf that lines up with the request is discarded whole, with no split. */
    model_reset();
    add_rec(0x10000, 0x30000, 0);
    add_leaf(0x10000, 2, 0x10000, 2 * PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, 2 * PAGE_SIZE) == 0,
          "discard: an aligned leaf is discarded whole");
    check(demote_calls == 0, "discard: an aligned leaf is not split");
    check(g_pages_total == 2, "discard: every page of the leaf is unaccounted");
    check(frame_puts == 1, "discard: the whole leaf releases one owner reference");

    /* A demote failure stops the transaction with the lock released. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    add_leaf(0x10000, 2, 0x10000, 2 * PAGE_SIZE);
    g_demote_reply = -12;
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == -12,
          "discard: a split failure is propagated");
    check(unlock_calls == 1 && g_locked == 0, "discard: the lock is released on failure");
    check(frame_puts == 0, "discard: nothing is released after a split failure");

    /* A hold failure stops the transaction before the unmap. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    g_hold_fail_va_set = 1; g_hold_fail_va = 0x10000;
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == -12,
          "discard: a hold failure is propagated");
    check(unmap_calls == 0, "discard: a held leaf is not unmapped after a hold failure");
    check(g_locked == 0, "discard: the lock is released after a hold failure");

    /* An unmap failure gives the lookup reference back and moves on. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    struct leaf *l2 = add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    l2->pcp = (void *)(uintptr_t)0xCACE;
    l2->unmap_rc = -12;
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: an unmap failure does not fail the advice");
    check(drop_calls == 1 && cache_puts == 1,
          "discard: the lookup reference is given back exactly once");
    check(release_calls == 0 && frame_puts == 0,
          "discard: no owner reference is released when the unmap failed");

    /* The cursor steps by the leaf, not by a page, when the leaf is wider. */
    model_reset();
    add_rec(0x10000, 0x30000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    add_leaf(0x11000, 0, 0x11000, PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, 2 * PAGE_SIZE) == 0,
          "discard: two adjacent leaves are discarded");
    check(frame_puts == 2 && g_pages_total == 2, "discard: both pages are released once");

    /* An absent leaf is skipped without touching the owner path. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0,
          "discard: an absent leaf is skipped");
    check(hold_calls == 0 && unmap_calls == 0 && release_calls == 0,
          "discard: an absent leaf touches nothing");
}

static void discard_envelope(void)
{
    /* The lock is held for the whole discard and released once. */
    model_reset();
    add_rec(0x10000, 0x11000, 0);
    add_leaf(0x10000, 0, 0x10000, PAGE_SIZE);
    check(a20_madvise_dontneed(g_mm_token, 0x10000, PAGE_SIZE) == 0, "envelope: discard ran");
    check(lock_calls == 1 && unlock_calls == 1, "envelope: one locked region");
    check(g_cache_puts_while_locked == 0, "envelope: the release path runs under the lock only");

    /* A null mm or zero length is refused before any lock. */
    model_reset();
    check(a20_madvise_dontneed(NULL, 0x10000, PAGE_SIZE) == -22, "envelope: null mm is EINVAL");
    check(a20_madvise_dontneed(g_mm_token, 0x10000, 0) == -22, "envelope: zero length is EINVAL");
    check(lock_calls == 0, "envelope: a refused call takes no lock");
}

static void native_lock_marker(void)
{
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_set_lock(g_mm_token, 0x10000, 0x12000, 1) == 0,
          "lock marker: a mapped range is accepted");
    check(g_set_flags_last == VM_NATIVE_LOCKED, "lock marker: the marker bit is used");
    check(g_set_flags_last != VM_LOCKED, "lock marker: the Linux mlock bit is not reused");
    check(g_set_on_last == 1, "lock marker: the range is marked on request");
    check(vma_at_calls == 1, "lock marker: the walk stops at the record end");

    model_reset();
    add_rec(0x10000, 0x11000, 0);
    check(a20_madvise_set_lock(g_mm_token, 0x10000, 0x12000, 1) == -12,
          "lock marker: an uncovered tail is ENOMEM");
    /* The marker is a reservation and is applied record by record, so the
     * covered prefix is marked before the uncovered tail is reported.  That is
     * the C reference's behaviour and it is preserved deliberately: the caller
     * is expected to pass a range it has already validated. */
    check(set_flags_calls == 1, "lock marker: the covered prefix is marked before the failure");

    model_reset();
    check(a20_madvise_set_lock(g_mm_token, 0x12000, 0x12000, 1) == -22,
          "lock marker: an empty range is EINVAL");
    check(a20_madvise_set_lock(g_mm_token, 0x10001, 0x12000, 1) == -22,
          "lock marker: an unaligned start is EINVAL");
    check(a20_madvise_set_lock(NULL, 0x10000, 0x12000, 1) == -22,
          "lock marker: a null mm is EINVAL");

    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_set_lock(g_mm_token, 0x10000, 0x12000, 0) == 0,
          "lock marker: clearing succeeds");
    check(g_set_on_last == 0, "lock marker: the clear is requested");
    check(g_set_flags_last == VM_NATIVE_LOCKED, "lock marker: the clear targets the marker bit");
}

static void vmo_export(void)
{
    model_reset();
    add_rec(0x10000, 0x12000, VM_VMO);
    uint32_t prot = 0;
    g_prot_reply = 7;
    void *vmo = a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2000, &prot);
    check(vmo == (void *)(uintptr_t)0x7B7B, "vmo: a fully covered VM_VMO record exports its vmo");
    check(ref_calls == 1, "vmo: exactly one reference is taken");
    check(prot == 7, "vmo: the mapping's protection is reported");
    check(lock_calls == 1 && unlock_calls == 1, "vmo: one locked region");
    check(g_locked == 0, "vmo: the lock is released before returning");

    /* Not a VMO record: no reference at all. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2000, &prot) == NULL,
          "vmo: a non-VMO record does not export");
    check(ref_calls == 0, "vmo: a non-VMO record takes no reference");

    /* A VMO record with no vmo behind it. */
    model_reset();
    add_rec(0x10000, 0x12000, VM_VMO);
    g_recs[0].vmo = NULL;
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2000, &prot) == NULL,
          "vmo: a record without a vmo exports nothing");
    check(ref_calls == 1, "vmo: the reference attempt is made once");
    check(lock_calls == 1 && unlock_calls == 1, "vmo: the failed export still unlocks");

    /* Partial coverage is refused. */
    model_reset();
    add_rec(0x10000, 0x11000, VM_VMO);
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2000, &prot) == NULL,
          "vmo: a partially covered range is refused");
    check(ref_calls == 0, "vmo: a refused range takes no reference");

    /* Alignment and length are checked before the lock. */
    model_reset();
    add_rec(0x10000, 0x12000, VM_VMO);
    check(a20_madvise_vmo_region(g_mm_token, 0x10001, 0x2000, &prot) == NULL,
          "vmo: an unaligned address is refused");
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2001, &prot) == NULL,
          "vmo: an unaligned length is refused");
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0, &prot) == NULL,
          "vmo: a zero length is refused");
    check(a20_madvise_vmo_region(NULL, 0x10000, 0x2000, &prot) == NULL,
          "vmo: a null mm is refused");
    check(lock_calls == 0, "vmo: a refused call takes no lock");

    /* A NULL prot_out is tolerated. */
    model_reset();
    add_rec(0x10000, 0x12000, VM_VMO);
    check(a20_madvise_vmo_region(g_mm_token, 0x10000, 0x2000, NULL) != NULL,
          "vmo: a null prot_out is tolerated");
}

static void pageout(void)
{
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == 0,
          "pageout: a plain range is handed to reclaim");
    check(pageout_calls == 1 && pageout_addr == 0x10000 && pageout_len == 0x2000,
          "pageout: the range is passed through");
    check(unlock_calls == 1 && g_locked == 0, "pageout: the lock is released before reclaim");

    /* A locked or kernel-owned record is refused. */
    model_reset();
    add_rec(0x10000, 0x12000, VM_LOCKED);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == -22,
          "pageout: a VM_LOCKED range is EINVAL");
    check(pageout_calls == 0, "pageout: a locked range is not reclaimed");

    model_reset();
    add_rec(0x10000, 0x12000, VM_PFNMAP);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == -22,
          "pageout: a VM_PFNMAP range is EINVAL");

    /* Another task's address space is refused. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    g_current_mm = (void *)(uintptr_t)0x0C0C;
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == -22,
          "pageout: another task's mm is EINVAL");
    check(lock_calls == 0 && pageout_calls == 0,
          "pageout: a foreign mm is refused before any work");

    /* An uncovered range is ENOMEM and never reaches reclaim. */
    model_reset();
    add_rec(0x12000, 0x13000, 0);
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == -12,
          "pageout: an uncovered range is ENOMEM");
    check(pageout_calls == 0, "pageout: an uncovered range is not reclaimed");

    /* A reclaim failure is propagated. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    pageout_reply = -12;
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == -12,
          "pageout: a reclaim failure is propagated");

    /* A positive reclaim count still answers success. */
    model_reset();
    add_rec(0x10000, 0x12000, 0);
    pageout_reply = 3;
    check(a20_madvise_dispatch(g_mm_token, 0x10000, 0x2000, 21) == 0,
          "pageout: a reclaimed count answers success");
}

int main(void)
{
    range_contract();
    coverage();
    advice_table();
    fork_flags_transaction();
    discard_owners();
    discard_envelope();
    native_lock_marker();
    vmo_export();
    pageout();

    if (failures) {
        printf("MADVISE_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MADVISE_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}
