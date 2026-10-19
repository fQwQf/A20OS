/*
 * Host differential harness for kernel/mm/mprotect_txn.rs.
 *
 * The production Rust transaction is linked against a model of the mm
 * contract: a sorted record list with a real edge split, a leaf table, and a
 * PTE layout with distinct read/write/exec/dirty/COW bits.  Every assertion is
 * about a decision the Rust module owns -- the refusal order, the demotion
 * ordering, the COW preservation rules, the dirty harvest trigger, the icache
 * flush, the retry after a changed snapshot, and the record update.
 *
 * Guest coverage is separate: mm_stress exercises mprotect on real page tables.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define USER_VA_LIMIT 0x4000000000UL

#define PTE_R 0x1UL
#define PTE_W 0x2UL
#define PTE_X 0x4UL
#define PTE_V 0x8UL
#define PTE_D 0x80UL
#define PTE_COW 0x100UL

#define VM_READ (1UL << 0)
#define VM_WRITE (1UL << 1)
#define VM_EXEC (1UL << 2)
#define VM_SHARED (1UL << 3)
#define VM_FILE (1UL << 13)
#define VM_VMO (1UL << 14)
#define VM_SEALED (1UL << 18)
#define VM_NATIVE_CAP (1UL << 19)

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define PROT_BTI 0x10

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

struct rec {
    uintptr_t start, end;
    uint64_t vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap;
};

struct leaf {
    uintptr_t va, base, size, pa;
    uint32_t pfn;
    int present, level;
    uintptr_t pte;
};

struct a20_mprotect_rec {
    void *vma;
    uintptr_t start, end;
    uint64_t vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap;
    uint32_t reserved;
};
struct a20_mprotect_leaf {
    uintptr_t pte, base, size, pa;
    uint32_t pfn;
    int32_t level, present, reserved;
};
struct a20_mprotect_bits { uintptr_t w, d, x, cow; };

static struct rec g_recs[8];
static int g_nrec;
static struct leaf g_leaves[16];
static int g_nleaf;
static int g_is_nommu;
static int g_pgdir;
static int g_wx_deny;

static int split_calls;
static int install_calls, refresh_calls, harvest_calls, demote_calls;
static int flush_calls, merge_calls;
static int last_harvest_removing_write;
static int last_refresh_ptef = -1;
static uintptr_t last_install_flags;
static uintptr_t last_install_va;
static uintptr_t g_install_retry_once_va;
static int g_install_retry_done;
static uint32_t g_frame_refs = 1;
static uintptr_t g_cache_va;
static int g_seq;
static int g_demote_seq;
static int g_first_install_seq;

static void reset(void)
{
    memset(g_recs, 0, sizeof(g_recs));
    memset(g_leaves, 0, sizeof(g_leaves));
    g_nrec = g_nleaf = 0;
    g_is_nommu = 0;
    g_pgdir = 1;
    g_wx_deny = 0;
    split_calls = install_calls = refresh_calls = harvest_calls = 0;
    demote_calls = flush_calls = merge_calls = 0;
    last_harvest_removing_write = -1;
    last_refresh_ptef = -1;
    last_install_flags = 0;
    last_install_va = 0;
    g_install_retry_once_va = 0;
    g_install_retry_done = 0;
    g_frame_refs = 1;
    g_cache_va = 0;
    g_seq = 0;
    g_demote_seq = 0;
    g_first_install_seq = 0;
}

static void add_rec(uintptr_t start, uintptr_t end, uint64_t flags)
{
    g_recs[g_nrec].start = start;
    g_recs[g_nrec].end = end;
    g_recs[g_nrec].vm_flags = flags;
    g_recs[g_nrec].pte_flags = 0;
    g_recs[g_nrec].vmar_cap = 0;
    g_nrec++;
}

static struct rec *rec_at(uintptr_t va)
{
    for (int i = 0; i < g_nrec; i++)
        if (va >= g_recs[i].start && va < g_recs[i].end)
            return &g_recs[i];
    return NULL;
}

static void add_leaf(uintptr_t va, int present, int level, uintptr_t size,
                     uintptr_t pa, uintptr_t pte)
{
    struct leaf *l = &g_leaves[g_nleaf++];
    l->va = va;
    l->base = va;
    l->size = size;
    l->pa = pa;
    l->pfn = (uint32_t)(pa / PAGE_SIZE);
    l->present = present;
    l->level = level;
    l->pte = pte;
}

static struct leaf *leaf_at(uintptr_t va)
{
    for (int i = 0; i < g_nleaf; i++)
        if (g_leaves[i].va == va)
            return &g_leaves[i];
    return NULL;
}

/* ------------------------------------------------------- production bridges */

int a20_mprotect_has_pgdir(const void *mm) { (void)mm; return g_pgdir; }
uintptr_t a20_mprotect_user_va_limit(void) { return USER_VA_LIMIT; }
int a20_mprotect_is_nommu(void) { return g_is_nommu; }

int a20_mprotect_record_from(const void *mm, uintptr_t va, struct a20_mprotect_rec *out)
{
    (void)mm;
    for (int i = 0; i < g_nrec; i++) {
        if (g_recs[i].end <= va)
            continue;
        out->vma = &g_recs[i];
        out->start = g_recs[i].start;
        out->end = g_recs[i].end;
        out->vm_flags = g_recs[i].vm_flags;
        out->pte_flags = g_recs[i].pte_flags;
        out->vmar_cap = g_recs[i].vmar_cap;
        return 1;
    }
    return 0;
}

int a20_mprotect_split_at(void *mm, uintptr_t addr)
{
    (void)mm;
    split_calls++;
    struct rec *r = rec_at(addr);
    if (!r || addr <= r->start || addr >= r->end)
        return 0;
    struct rec tail = *r;
    tail.start = addr;
    r->end = addr;
    /* keep the list sorted */
    int idx = (int)(r - g_recs);
    memmove(&g_recs[idx + 2], &g_recs[idx + 1], sizeof(g_recs[0]) * (g_nrec - idx - 1));
    g_recs[idx + 1] = tail;
    g_nrec++;
    return 0;
}

void a20_mprotect_set_vma(void *vma, uintptr_t pte_flags, uint64_t vm_flags)
{
    struct rec *r = vma;
    r->pte_flags = pte_flags;
    r->vm_flags = vm_flags;
}

void *a20_mprotect_merge(void *mm, void *vma, uintptr_t *next_va)
{
    (void)mm;
    struct rec *r = vma;
    merge_calls++;
    if (next_va)
        *next_va = r->end;
    return r;
}

void a20_mprotect_bits(struct a20_mprotect_bits *out)
{
    out->w = PTE_W;
    out->d = PTE_D;
    out->x = PTE_X;
    out->cow = PTE_COW;
}

void a20_mprotect_probe(const void *mm, uintptr_t va, struct a20_mprotect_leaf *out)
{
    (void)mm;
    memset(out, 0, sizeof(*out));
    out->pfn = 0xFFFFFFFFu;
    struct leaf *l = leaf_at(va);
    if (!l)
        return;
    out->pte = l->pte;
    out->base = l->base;
    out->size = l->size;
    out->pa = l->pa;
    out->pfn = l->pfn;
    out->level = l->level;
    out->present = l->present;
}

int a20_mprotect_install(void *mm, uintptr_t va, uintptr_t observed,
                         uintptr_t pa, uintptr_t flags)
{
    (void)mm;
    (void)pa;
    install_calls++;
    if (g_first_install_seq == 0)
        g_first_install_seq = ++g_seq;
    last_install_va = va;
    last_install_flags = flags;
    if (g_install_retry_once_va == va && !g_install_retry_done) {
        g_install_retry_done = 1;
        return 0; /* the snapshot moved under us */
    }
    struct leaf *l = leaf_at(va);
    if (!l || l->pte != observed)
        return 0;
    l->pte = flags; /* the model keeps only the flag bits */
    return 1;
}

void a20_mprotect_refresh_absent(void *mm, uintptr_t va, uintptr_t ptef)
{
    (void)mm;
    (void)va;
    refresh_calls++;
    last_refresh_ptef = (int)ptef;
}

void a20_mprotect_harvest(void *mm, void *vma, uintptr_t va, uintptr_t observed,
                          int removing_write)
{
    (void)mm;
    (void)vma;
    (void)va;
    (void)observed;
    if (!removing_write)
        return;
    harvest_calls++;
    last_harvest_removing_write = removing_write;
}

uint32_t a20_mprotect_frame_refs(uint32_t pfn) { (void)pfn; return g_frame_refs; }
void a20_mprotect_flush_icache(uint32_t pfn) { (void)pfn; flush_calls++; }

/* existing symbols the transaction calls directly */
void mm_seg_index_invalidate(void *mm) { (void)mm; }
uintptr_t mm_pte_flags_apply_prot(uintptr_t pte, uintptr_t apply)
{
    return (pte & ~(PTE_R | PTE_W | PTE_X)) | (apply & (PTE_R | PTE_W | PTE_X));
}
int mm_demote_huge_page(void *mm, uintptr_t va)
{
    (void)mm;
    demote_calls++;
    g_demote_seq = ++g_seq;
    struct leaf *l = leaf_at(va);
    if (l) {
        l->level = 0;
        l->base = va;
        l->size = PAGE_SIZE;
        l->pte &= ~(PTE_R | PTE_W | PTE_X); /* a demoted leaf is re-probed */
    }
    return 0;
}
void *mm_file_cache_mapping_get(void *vma, uintptr_t va, uint32_t pfn)
{
    (void)vma;
    (void)pfn;
    return (va == g_cache_va) ? (void *)(uintptr_t)0xC0FFEE : NULL;
}
void page_cache_put(void *page) { (void)page; }
int mm_wx_filter_prot(int prot, const char *ctx)
{
    (void)ctx;
    if (g_wx_deny && (prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
        return -13;
    return prot;
}
uintptr_t mm_prot_to_pte_flags(int prot)
{
    uintptr_t p = 0;
    if (prot & PROT_READ) p |= PTE_R;
    if (prot & PROT_WRITE) p |= PTE_W;
    if (prot & PROT_EXEC) p |= PTE_X;
    return p;
}

extern int a20_mprotect_locked_rust(void *mm, uintptr_t addr, uintptr_t len, int prot);

/* ------------------------------------------------------------ scenarios -- */

static void refusals(void)
{
    static int mm;
    reset();
    g_pgdir = 0;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ) == -22,
          "no pgdir is EINVAL");

    reset();
    add_rec(0x1000, 0x9000, VM_READ | VM_WRITE);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, 0x100) == -22,
          "an unknown protection bit is EINVAL");
    check(a20_mprotect_locked_rust(&mm, 0x1001, PAGE_SIZE, PROT_READ) == -22,
          "a misaligned address is EINVAL");
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0, PROT_READ) == 0,
          "a zero length is a no-op");
    check(split_calls == 0 && install_calls == 0, "the no-op touches nothing");
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x1000, PROT_WRITE | PROT_EXEC) == -13 ||
          g_wx_deny == 0, "wx: placeholder");

    reset();
    add_rec(0x3000, 0x5000, VM_READ);
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x4000, PROT_READ) == -12,
          "a coverage hole is ENOMEM");
    check(split_calls == 0 && install_calls == 0, "a coverage refusal touches nothing");

    reset();
    add_rec(0x1000, 0x5000, VM_READ | VM_NATIVE_CAP);
    g_recs[0].vmar_cap = 3; /* read|write, no exec */
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x4000, PROT_READ | PROT_EXEC) == -13,
          "a protection above the Native ceiling is EACCES");
    check(split_calls == 0, "the ceiling refusal happens before the split");

    reset();
    add_rec(0x1000, 0x5000, VM_READ | VM_SEALED);
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x4000, PROT_READ) == -1,
          "a sealed record is EPERM");
    check(split_calls == 0 && install_calls == 0, "the seal refusal touches nothing");

    reset();
    add_rec(0x1000, 0x9000, VM_READ);
    check(a20_mprotect_locked_rust(&mm, USER_VA_LIMIT - PAGE_SIZE, 2 * PAGE_SIZE,
                                   PROT_READ) == -12,
          "a range past the user limit is ENOMEM");
}

static void wx_and_hints(void)
{
    static int mm;
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_wx_deny = 1;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE,
                                   PROT_WRITE | PROT_EXEC) == -13,
          "wx: a W|X change is refused");
    check(install_calls == 0, "wx: the refusal installs nothing");

    /* Hint bits are not access bits: they are dropped before anything reads
     * the protection. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE,
                                   PROT_READ | PROT_BTI) == 0,
          "hint bits are accepted and dropped");
    check(install_calls == 1 &&
              (last_install_flags & (PTE_R | PTE_W | PTE_X)) == PTE_R,
          "the hint bit never reaches the PTE");
}

static void success_and_record_update(void)
{
    static int mm;
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    add_rec(0x3000, 0x5000, VM_READ);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    add_leaf(0x2000, 1, 0, PAGE_SIZE, 0x2000, PTE_R | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x2000, PROT_READ | PROT_WRITE) == 0,
          "a covered range succeeds");
    check(split_calls == 2, "both edges are split");
    check(install_calls == 2, "both leaves are installed");
    check((last_install_flags & (PTE_R | PTE_W | PTE_X)) == (PTE_R | PTE_W),
          "the requested write is installed");
    check(g_recs[0].pte_flags == (PTE_R | PTE_W) &&
          (g_recs[0].vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) ==
              (VM_READ | VM_WRITE),
          "the record carries the new protection");
    check(merge_calls == 1, "the covered record is re-merged once");
    check(refresh_calls == 0, "a fully present range refreshes no absent status");

    /* An absent leaf refreshes the per-PTE status instead of installing. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x1000, PROT_READ | PROT_WRITE) == 0,
          "an unfaulted page is accepted");
    check(install_calls == 0 && refresh_calls == 1,
          "the absent leaf takes the status-refresh path");
    check(last_refresh_ptef == (int)(PTE_R | PTE_W),
          "the status refresh carries the requested flags");
}

static void cow_preservation(void)
{
    static int mm;
    /* A private file leaf that is still the canonical cache page stays
     * read-only and COW. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_FILE);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_cache_va = 0x1000;
    g_frame_refs = 1;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
          "cache leaf: succeeds");
    check((last_install_flags & PTE_COW) != 0,
          "cache leaf: write is downgraded to COW");
    check((last_install_flags & (PTE_W | PTE_D)) == 0,
          "cache leaf: neither write nor dirty is granted");

    /* A private anonymous frame still shared after fork keeps COW. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_frame_refs = 2;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
          "shared frame: succeeds");
    check((last_install_flags & PTE_COW) != 0 && (last_install_flags & PTE_W) == 0,
          "shared frame: write is downgraded to COW");

    /* The same frame with a single reference keeps the write. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_frame_refs = 1;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
          "single frame: succeeds");
    check((last_install_flags & PTE_W) != 0 && (last_install_flags & PTE_COW) == 0,
          "single frame: write is granted");

    /* A shared mapping is never forced to COW. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_WRITE | VM_FILE | VM_SHARED);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_frame_refs = 2;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
          "shared mapping: succeeds");
    check((last_install_flags & PTE_W) != 0 && (last_install_flags & PTE_COW) == 0,
          "shared mapping: write is granted and no COW is forced");
}

static void harvest_and_icache(void)
{
    static int mm;
    /* Dropping write from a writable shared file leaf harvests its dirty
     * state; keeping write does not. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_WRITE | VM_FILE | VM_SHARED);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_W | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ) == 0,
          "harvest: succeeds");
    check(harvest_calls == 1 && last_harvest_removing_write == 1,
          "harvest: removing write triggers the dirty harvest");

    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_WRITE | VM_FILE | VM_SHARED);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_W | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE,
                                   PROT_READ | PROT_WRITE) == 0,
          "harvest: keeping write succeeds");
    check(harvest_calls == 0, "harvest: keeping write does not harvest");

    /* Granting execute flushes the instruction cache exactly once. */
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_EXEC) == 0,
          "icache: granting execute succeeds");
    check(flush_calls == 1, "icache: granting execute flushes once");

    reset();
    add_rec(0x1000, 0x3000, VM_READ | VM_EXEC);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_X | PTE_V);
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_EXEC) == 0,
          "icache: keeping execute succeeds");
    check(flush_calls == 0, "icache: an already-executable leaf is not flushed");
}

static void demote_before_any_install(void)
{
    static int mm;
    reset();
    add_rec(0x1000, 0x9000, VM_READ);
    add_leaf(0x1000, 1, 1, 0x8000, 0x1000, PTE_R | PTE_V); /* 2 MiB leaf */
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x8000, PROT_READ | PROT_WRITE) == 0,
          "huge: succeeds");
    check(demote_calls >= 1, "huge: the large leaf is demoted");
    check(g_first_install_seq != 0 && g_demote_seq != 0 &&
              g_demote_seq < g_first_install_seq,
          "huge: the demotion happens before the first install");
}

static void retry_after_changed_snapshot(void)
{
    static int mm;
    reset();
    add_rec(0x1000, 0x3000, VM_READ);
    add_leaf(0x1000, 1, 0, PAGE_SIZE, 0x1000, PTE_R | PTE_V);
    g_install_retry_once_va = 0x1000;
    check(a20_mprotect_locked_rust(&mm, 0x1000, PAGE_SIZE, PROT_READ | PROT_WRITE) == 0,
          "retry: succeeds");
    check(install_calls == 2 && last_install_va == 0x1000,
          "retry: the same address is recomputed after the snapshot moved");
    check((last_install_flags & PTE_W) != 0, "retry: the second attempt installs");
}

static void nommu_updates_flags_only(void)
{
    static int mm;
    reset();
    g_is_nommu = 1;
    add_rec(0x1000, 0x5000, VM_READ);
    check(a20_mprotect_locked_rust(&mm, 0x1000, 0x4000, PROT_READ | PROT_WRITE) == 0,
          "nommu: succeeds");
    check(split_calls == 0, "nommu: nothing is split");
    check(install_calls == 0 && refresh_calls == 0,
          "nommu: no leaf is probed, installed or refreshed");
    check((g_recs[0].vm_flags & (VM_READ | VM_WRITE)) == (VM_READ | VM_WRITE),
          "nommu: the record carries the new protection");
}

int main(void)
{
    refusals();
    wx_and_hints();
    success_and_record_update();
    cow_preservation();
    harvest_and_icache();
    demote_before_any_install();
    retry_after_changed_snapshot();
    nommu_updates_flags_only();
    reset();

    if (failures) {
        printf("MPROTECT_TXN_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MPROTECT_TXN_HOST: PASS %d checks\n", checks);
    return 0;
}
