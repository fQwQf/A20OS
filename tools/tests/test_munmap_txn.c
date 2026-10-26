/*
 * Host differential and failure-injection harness for
 * kernel/mm/munmap_txn.rs.
 *
 * The production Rust transaction is linked against a model of the mm
 * contract: a sorted VMA list, a leaf table, and counters for every bridge it
 * is allowed to call.  Every assertion is about a decision the Rust module
 * owns -- which clip it took, which release class it chose, what it released
 * before a failure, and what it refused to touch.
 *
 * This is a state-machine oracle, not a kernel.  It does not establish real
 * page-table, allocator or guest behavior; the guest cases do that.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define USER_VA_LIMIT 0x4000000000UL

#define VM_READ (1UL << 0)
#define VM_WRITE (1UL << 1)
#define VM_SHARED (1UL << 3)
#define VM_FILE (1UL << 13)
#define VM_VMO (1UL << 14)
#define VM_LOCKED (1UL << 15)
#define VM_SYSV_SHM (1UL << 16)
#define VM_PFNMAP (1UL << 17)
#define VM_SEALED (1UL << 18)

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


/* --------------------------------------------------------------- model --- */

struct stub_vma {
    uintptr_t start, end;
    uint64_t vm_flags;
    uint64_t backing_offset;
    void *file_vnode;
    struct stub_vma *next;
};

struct stub_leaf {
    uintptr_t va;
    uintptr_t base, size;
    uintptr_t pa;
    uint32_t pfn;
    int present;
    int swap;
    int level;
};

struct stub_mm {
    struct stub_vma *mmap;
    uintptr_t total_vm;
    uintptr_t locked_vm;
    int has_pgdir;
};

static struct stub_mm g_mm;
static struct stub_vma g_vmas[8];
static struct stub_leaf g_leaves[16];
static int g_nvma, g_nleaf;

static int idx_clear_calls, drop_whole_calls, cut_head_calls, cut_tail_calls;
static int cut_middle_calls, account_calls, demote_calls, retire_calls;
static int hold_calls, unmap_calls, release_calls, page_put_calls;
static int harvest_calls, harvest_shared_file;
static uintptr_t last_clip_start, last_clip_end;
static uintptr_t last_freed_pages, last_locked_bytes;
static int cut_middle_fail;      /* inject -ENOMEM from the middle cut */
static int g_is_nommu;
static int flat_release_calls;
static uintptr_t cache_owned_va; /* this leaf is owned by the page cache */
static int hold_fail_at;         /* inject -ENOMEM on the Nth hold (1-based) */
static int leaf_release_frames, leaf_release_pages, leaf_release_rss;
static uintptr_t demote_va;

static void reset(void)
{
    memset(&g_mm, 0, sizeof(g_mm));
    memset(g_vmas, 0, sizeof(g_vmas));
    memset(g_leaves, 0, sizeof(g_leaves));
    g_mm.has_pgdir = 1;
    g_nvma = g_nleaf = 0;
    idx_clear_calls = drop_whole_calls = cut_head_calls = cut_tail_calls = 0;
    cut_middle_calls = account_calls = demote_calls = retire_calls = 0;
    hold_calls = unmap_calls = release_calls = page_put_calls = 0;
    harvest_calls = harvest_shared_file = 0;
    last_clip_start = last_clip_end = 0;
    last_freed_pages = last_locked_bytes = 0;
    cut_middle_fail = 0;
    hold_fail_at = 0;
    g_is_nommu = 0;
    flat_release_calls = 0;
    cache_owned_va = 0;
    leaf_release_frames = leaf_release_pages = leaf_release_rss = 0;
    demote_va = 0;
}

static struct stub_vma *add_vma(uintptr_t start, uintptr_t end, uint64_t flags)
{
    struct stub_vma *v = &g_vmas[g_nvma++];
    v->start = start;
    v->end = end;
    v->vm_flags = flags;
    v->backing_offset = 0;
    v->file_vnode = NULL;
    v->next = NULL;
    for (int i = 0; i < g_nvma - 1; i++) {
        if (g_vmas[i].start > start) {
            memmove(&g_vmas[i + 1], &g_vmas[i], sizeof(g_vmas[0]) * (g_nvma - 1 - i));
            g_vmas[i] = *v;
            break;
        }
    }
    g_mm.mmap = &g_vmas[0];
    for (int i = 0; i + 1 < g_nvma; i++)
        g_vmas[i].next = &g_vmas[i + 1];
    return &g_vmas[g_nvma - 1];
}

static struct stub_leaf *add_leaf(uintptr_t va, int present, int swap, int level,
                                  uintptr_t base, uintptr_t size, uintptr_t pa)
{
    struct stub_leaf *l = &g_leaves[g_nleaf++];
    l->va = va;
    l->present = present;
    l->swap = swap;
    l->level = level;
    l->base = base;
    l->size = size;
    l->pa = pa;
    l->pfn = (uint32_t)(pa / PAGE_SIZE);
    return l;
}

/* Leaves are matched by the range they cover, the way the real walk returns
 * the entry that owns an address rather than an exact-address hit. */
static struct stub_leaf *find_leaf(uintptr_t va)
{
    struct stub_leaf *best = NULL;
    for (int i = 0; i < g_nleaf; i++) {
        struct stub_leaf *l = &g_leaves[i];
        if (l->size == 0 || va < l->base || va >= l->base + l->size)
            continue;
        if (!best || l->level > best->level)
            best = l;
    }
    return best;
}

/* ------------------------------------------------- production bridges --- */

struct a20_munmap_vma_info {
    uintptr_t start, end;
    uint64_t vm_flags;
    uint64_t backing_offset;
    void *file_vnode;
};

struct a20_munmap_vma_bounds {
    uintptr_t start, end;
    uint64_t vm_flags;
};

struct a20_munmap_entry {
    void *vma;
    uintptr_t start, end;
    uint64_t vm_flags, backing_offset;
    void *file_vnode;
};

struct a20_munmap_leaf {
    uintptr_t aux, base, size;
    uint32_t pfn;
    int32_t level, present, swap;
};

void mm_seg_index_invalidate(void *mm) { (void)mm; }
void mm_seg_index_clear(void *mm) { (void)mm; idx_clear_calls++; }

int a20_munmap_has_pgdir(const void *mm) { return ((const struct stub_mm *)mm)->has_pgdir; }
uintptr_t a20_munmap_user_va_limit(void) { return USER_VA_LIMIT; }

void *a20_munmap_vma_first(const void *mm) { return ((const struct stub_mm *)mm)->mmap; }
void *a20_munmap_vma_next(void *v) { return v ? ((struct stub_vma *)v)->next : NULL; }

void a20_munmap_vma_info(void *vma, struct a20_munmap_vma_info *out)
{
    struct stub_vma *v = vma;
    if (!v || !out) return;
    out->start = v->start;
    out->end = v->end;
    out->vm_flags = v->vm_flags;
    out->backing_offset = v->backing_offset;
    out->file_vnode = v->file_vnode;
}

void a20_munmap_vma_bounds(void *vma, struct a20_munmap_vma_bounds *out)
{
    struct stub_vma *v = vma;
    if (!v || !out) return;
    out->start = v->start;
    out->end = v->end;
    out->vm_flags = v->vm_flags;
}

int a20_munmap_collect(void *mm, uintptr_t addr, uintptr_t end,
                       struct a20_munmap_entry *out, uintptr_t cap)
{
    struct stub_mm *m = mm;
    uintptr_t n = 0;
    for (struct stub_vma *v = m->mmap; v; v = v->next) {
        if (v->start >= end) break;
        if (v->end <= addr) continue;
        if (n == cap) return -28;
        struct a20_munmap_entry *e = &out[n++];
        e->vma = v; e->start = v->start; e->end = v->end;
        e->vm_flags = v->vm_flags; e->backing_offset = v->backing_offset;
        e->file_vnode = v->file_vnode;
    }
    return (int)n;
}

void a20_munmap_account(void *mm, uintptr_t freed_pages, uintptr_t locked_bytes)
{
    struct stub_mm *m = mm;
    account_calls++;
    last_freed_pages = freed_pages;
    last_locked_bytes = locked_bytes;
    m->total_vm = (m->total_vm > freed_pages) ? m->total_vm - freed_pages : 0;
    if (locked_bytes)
        m->locked_vm = (m->locked_vm >= locked_bytes) ? m->locked_vm - locked_bytes : 0;
}

static void unlink_vma(struct stub_vma *v)
{
    struct stub_vma **pp = &g_mm.mmap;
    while (*pp && *pp != v)
        pp = &(*pp)->next;
    if (*pp)
        *pp = v->next;
}

void a20_munmap_drop_whole(void *mm, void *vma, uintptr_t s, uintptr_t e)
{
    (void)mm; (void)s; (void)e;
    drop_whole_calls++;
    unlink_vma(vma);
}

void a20_munmap_cut_head(void *mm, void *vma, uintptr_t clip_end)
{
    (void)mm;
    struct stub_vma *v = vma;
    cut_head_calls++;
    last_clip_end = clip_end;
    v->backing_offset += clip_end - v->start;
    v->start = clip_end;
}

void a20_munmap_cut_tail(void *mm, void *vma, uintptr_t clip_start)
{
    (void)mm;
    struct stub_vma *v = vma;
    cut_tail_calls++;
    last_clip_start = clip_start;
    v->end = clip_start;
}

int a20_munmap_cut_middle(void *mm, void *vma, uintptr_t clip_start, uintptr_t clip_end)
{
    (void)mm;
    struct stub_vma *v = vma;
    cut_middle_calls++;
    last_clip_start = clip_start;
    last_clip_end = clip_end;
    if (cut_middle_fail)
        return -12;
    v->end = clip_start;
    return 0;
}

int a20_munmap_is_nommu(void) { return g_is_nommu; }
void a20_munmap_release_flat(void *mm, void *vma, uintptr_t s, uintptr_t e)
{ (void)mm; (void)vma; (void)s; (void)e; flat_release_calls++; }

void a20_munmap_leaf_probe(void *mm, uintptr_t va, struct a20_munmap_leaf *out)
{
    (void)mm;
    memset(out, 0, sizeof(*out));
    out->pfn = 0xFFFFFFFFu;
    struct stub_leaf *l = find_leaf(va);
    if (!l) return;
    out->aux = (uintptr_t)(l->present ? l->pa | 1 : l->pa);
    out->base = l->base;
    out->size = l->size;
    out->pfn = l->pfn;
    out->level = l->level;
    out->present = l->present;
    out->swap = l->swap;
}

void a20_munmap_retire_reservation(void *mm, uintptr_t va) { (void)mm; (void)va; retire_calls++; }

int a20_munmap_demote(void *mm, uintptr_t va)
{
    (void)mm;
    demote_calls++;
    demote_va = va;
    struct stub_leaf *l = find_leaf(va);
    if (l) { l->level = 0; l->base = va; l->size = PAGE_SIZE; }
    return 0;
}

void a20_munmap_leaf_harvest(void *mm, void *vma, uintptr_t va, uintptr_t cs,
                             uintptr_t ce, const struct a20_munmap_leaf *leaf,
                             int shared_file)
{
    (void)mm; (void)vma; (void)va; (void)cs; (void)ce; (void)leaf;
    if (!shared_file)
        return;
    harvest_calls++;
    harvest_shared_file = shared_file;
}

int a20_munmap_leaf_hold(void *mm, void *vma, uintptr_t va, uint32_t pfn,
                         int no_frame_ref, void **out_pcp)
{
    (void)mm; (void)vma; (void)va; (void)pfn; (void)no_frame_ref;
    hold_calls++;
    if (hold_fail_at && hold_calls == hold_fail_at)
        return -12;
    /* A page-cache-owned leaf is recognised by its PFN, not by a VMA-wide
     * flag, so the model designates it per leaf. */
    *out_pcp = (va == cache_owned_va) ? (void *)(uintptr_t)0xC0FFEE : NULL;
    return 0;
}

int a20_munmap_leaf_unmap(void *mm, uintptr_t va, struct a20_munmap_leaf *out)
{
    (void)mm;
    unmap_calls++;
    struct stub_leaf *l = find_leaf(va);
    if (!l) {
        out->aux = 0; out->base = va; out->size = PAGE_SIZE;
        return -1;
    }
    out->aux = l->present ? l->pa : 0;
    out->base = l->base;
    out->size = l->size;
    l->present = 0;
    l->swap = 0;
    return 0;
}

void a20_munmap_leaf_release(void *mm, int no_frame_ref, uintptr_t pa,
                             uintptr_t base, uintptr_t size, void *pcp)
{
    (void)mm; (void)base;
    release_calls++;
    if (!pa)
        return;
    if (pcp) leaf_release_pages++;
    else if (!no_frame_ref) leaf_release_frames++;
    leaf_release_rss += (int)(size / PAGE_SIZE);
}

void a20_munmap_page_put(void *pcp) { if (pcp) page_put_calls++; }

extern int a20_munmap_rust(void *mm, uintptr_t addr, uintptr_t len);

/* ------------------------------------------------------------ scenarios -- */

static void whole_record(void)
{
    reset();
    add_vma(0x1000, 0x5000, VM_READ | VM_WRITE);
    for (uintptr_t va = 0x1000; va < 0x5000; va += PAGE_SIZE)
        add_leaf(va, 1, 0, 0, va, PAGE_SIZE, va + 0x80000000UL);
    check(a20_munmap_rust(&g_mm, 0x1000, 0x4000) == 0, "whole: returns 0");
    check(drop_whole_calls == 1, "whole: the record is dropped once");
    check(cut_head_calls + cut_tail_calls + cut_middle_calls == 0,
          "whole: no partial cut runs");
    check(account_calls == 1 && last_freed_pages == 4, "whole: four pages accounted");
    check(last_locked_bytes == 0, "whole: an unlocked record charges no locked bytes");
    check(leaf_release_frames == 4, "whole: four frames released");
    check(leaf_release_rss == 4, "whole: rss drops by four pages");
    check(idx_clear_calls == 1, "whole: the lookup index is cleared once");
    check(g_mm.mmap == NULL, "whole: the list is empty");
}

static void partial_cuts(void)
{
    reset();
    add_vma(0x1000, 0x5000, VM_READ | VM_WRITE | VM_LOCKED);
    add_leaf(0x1000, 1, 0, 0, 0x1000, PAGE_SIZE, 0x1000);
    add_leaf(0x2000, 1, 0, 0, 0x2000, PAGE_SIZE, 0x2000);
    g_mm.total_vm = 4;
    g_mm.locked_vm = 0x4000;
    check(a20_munmap_rust(&g_mm, 0x1000, 0x2000) == 0, "head: returns 0");
    check(cut_head_calls == 1 && last_clip_end == 0x3000, "head: cut at the range end");
    check(last_locked_bytes == 0x2000, "head: a locked record reports its locked bytes");
    check(g_mm.locked_vm == 0x2000, "head: locked_vm is decremented");

    reset();
    add_vma(0x1000, 0x5000, VM_READ | VM_WRITE);
    add_leaf(0x3000, 1, 0, 0, 0x3000, PAGE_SIZE, 0x3000);
    check(a20_munmap_rust(&g_mm, 0x3000, 0x2000) == 0, "tail: returns 0");
    check(cut_tail_calls == 1 && last_clip_start == 0x3000, "tail: cut at the range start");

    reset();
    add_vma(0x1000, 0x5000, VM_READ | VM_WRITE);
    add_leaf(0x2000, 1, 0, 0, 0x2000, PAGE_SIZE, 0x2000);
    add_leaf(0x3000, 1, 0, 0, 0x3000, PAGE_SIZE, 0x3000);
    check(a20_munmap_rust(&g_mm, 0x2000, 0x2000) == 0, "middle: returns 0");
    check(cut_middle_calls == 1, "middle: the record is split");
    check(last_clip_start == 0x2000 && last_clip_end == 0x4000,
          "middle: both clip bounds reach the cut");
    check(g_vmas[0].end == 0x2000, "middle: the head keeps its side");
}

static void refusals_touch_nothing(void)
{
    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE | VM_SYSV_SHM);
    check(a20_munmap_rust(&g_mm, 0x2000, 0x2000) == -22,
          "sysv: a partial segment is EINVAL");
    check(release_calls == 0 && drop_whole_calls == 0 && cut_middle_calls == 0,
          "sysv: the refusal releases and cuts nothing");

    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE | VM_SEALED);
    check(a20_munmap_rust(&g_mm, 0x1000, 0x8000) == -1,
          "sealed: the range is EPERM");
    check(release_calls == 0, "sealed: nothing is released");

    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE);
    check(a20_munmap_rust(&g_mm, 0x1001, 0x1000) == -22, "misaligned address is EINVAL");
    check(a20_munmap_rust(&g_mm, USER_VA_LIMIT - PAGE_SIZE, 2 * PAGE_SIZE) == -22,
          "a range past the user limit is EINVAL");
    check(a20_munmap_rust(&g_mm, 0x1000, 0) == 0, "a zero length is a no-op");
    check(account_calls == 0, "no refusal accounts anything");
}

static void release_classes(void)
{
    /* A PFNMAP leaf takes no frame reference, so nothing is put for it. */
    reset();
    add_vma(0x1000, 0x3000, VM_READ | VM_WRITE | VM_PFNMAP);
    add_leaf(0x1000, 1, 0, 0, 0x1000, PAGE_SIZE, 0x1000);
    check(a20_munmap_rust(&g_mm, 0x1000, PAGE_SIZE) == 0, "pfnmap: returns 0");
    check(leaf_release_frames == 0, "pfnmap: no frame is put");
    check(leaf_release_rss == 1, "pfnmap: rss still drops");

    /* A page-cache-owned leaf drops the temporary lookup and the mapping pin. */
    reset();
    add_vma(0x1000, 0x3000, VM_READ | VM_SHARED | VM_FILE);
    add_leaf(0x1000, 1, 0, 0, 0x1000, PAGE_SIZE, 0x1000);
    cache_owned_va = 0x1000;
    check(a20_munmap_rust(&g_mm, 0x1000, PAGE_SIZE) == 0, "cache leaf: returns 0");
    check(leaf_release_pages == 1, "cache leaf: the page-cache branch is taken");
    check(leaf_release_frames == 0, "cache leaf: the frame branch is not taken");
    check(harvest_calls == 1 && harvest_shared_file == 1,
          "cache leaf: a writable shared file leaf is harvested");

    /* A swap leaf has no frame to hold and no physical address to release. */
    reset();
    add_vma(0x1000, 0x3000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 0, 1, 0, 0x1000, PAGE_SIZE, 0x5000);
    check(a20_munmap_rust(&g_mm, 0x1000, PAGE_SIZE) == 0, "swap leaf: returns 0");
    check(hold_calls == 0, "swap leaf: nothing is held");
    check(unmap_calls == 1, "swap leaf: the leaf is unmapped");
    check(leaf_release_frames == 0 && leaf_release_pages == 0,
          "swap leaf: nothing is released");
    check(harvest_calls == 0, "swap leaf: no dirty harvest");
}

static void huge_leaf_is_demoted_first(void)
{
    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 1, 0, 1, 0x1000, 0x8000, 0x1000);   /* 2 MiB leaf, straddles */
    check(a20_munmap_rust(&g_mm, 0x2000, PAGE_SIZE) == 0, "huge: returns 0");
    check(demote_calls == 1 && demote_va == 0x2000,
          "huge: the straddling leaf is demoted before anything is released");
    check(release_calls == 1, "huge: the demoted base leaf is released");
}

static void never_faulted_reservation_is_retired(void)
{
    reset();
    add_vma(0x1000, 0x3000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 0, 0, 0, 0x1000, PAGE_SIZE, 0);  /* reserved, no leaf */
    check(a20_munmap_rust(&g_mm, 0x1000, PAGE_SIZE) == 0, "reservation: returns 0");
    check(retire_calls == 1, "reservation: the status mark is retired");
    check(unmap_calls == 0, "reservation: there is no leaf to unmap");

    /* A whole absent subtree (level > 0) has no level-0 mark to retire. */
    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 0, 0, 2, 0x1000, 0x4000, 0);
    check(a20_munmap_rust(&g_mm, 0x1000, 0x4000) == 0, "absent subtree: returns 0");
    check(retire_calls == 0, "absent subtree: no level-0 mark is retired");
}

static void failure_paths(void)
{
    /* The middle cut is the only fallible step after the release loop.  Its
     * failure must be reported with the pages already released and no record
     * surgery performed. */
    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE);
    add_leaf(0x2000, 1, 0, 0, 0x2000, PAGE_SIZE, 0x2000);
    add_leaf(0x3000, 1, 0, 0, 0x3000, PAGE_SIZE, 0x3000);
    cut_middle_fail = 1;
    check(a20_munmap_rust(&g_mm, 0x2000, 0x2000) == -12,
          "cut failure: the error reaches the caller");
    check(cut_middle_calls == 1, "cut failure: the cut was attempted");
    check(leaf_release_frames == 2, "cut failure: the clip was already released");
    check(account_calls == 1, "cut failure: the clip was already accounted");
    check(g_vmas[0].end == 0x9000, "cut failure: the record was not split");
    check(idx_clear_calls == 0, "cut failure: the index is left for the next lookup");

    /* A failed TLB hold stops the transaction after the earlier leaves. */
    reset();
    add_vma(0x1000, 0x9000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 1, 0, 0, 0x1000, PAGE_SIZE, 0x1000);
    add_leaf(0x2000, 1, 0, 0, 0x2000, PAGE_SIZE, 0x2000);
    add_leaf(0x3000, 1, 0, 0, 0x3000, PAGE_SIZE, 0x3000);
    hold_fail_at = 2;
    check(a20_munmap_rust(&g_mm, 0x1000, 0x3000) == -12,
          "hold failure: the error reaches the caller");
    check(leaf_release_frames == 1, "hold failure: only the earlier leaf was released");
    check(drop_whole_calls == 0 && cut_head_calls == 0,
          "hold failure: no record surgery happened");
    check(idx_clear_calls == 0, "hold failure: the index is not cleared");
}

static void multi_record(void)
{
    reset();
    add_vma(0x1000, 0x3000, VM_READ | VM_WRITE);
    add_vma(0x4000, 0x6000, VM_READ | VM_WRITE);
    add_leaf(0x1000, 1, 0, 0, 0x1000, PAGE_SIZE, 0x1000);
    add_leaf(0x4000, 1, 0, 0, 0x4000, PAGE_SIZE, 0x4000);
    check(a20_munmap_rust(&g_mm, 0x1000, 0x5000) == 0, "multi: returns 0");
    check(drop_whole_calls == 2, "multi: both fully covered records are dropped");
    check(leaf_release_frames == 2, "multi: both records release their leaf");
    check(g_mm.mmap == NULL, "multi: the list is empty");
}

static void nommu_releases_the_flat_allocation(void)
{
    reset();
    g_is_nommu = 1;
    add_vma(0x1000, 0x3000, VM_READ | VM_WRITE);
    check(a20_munmap_rust(&g_mm, 0x1000, 0x2000) == 0, "nommu: returns 0");
    check(flat_release_calls == 1, "nommu: the flat allocation is released");
    check(release_calls == 0 && unmap_calls == 0 && hold_calls == 0,
          "nommu: no leaf operation is attempted");
    check(drop_whole_calls == 1, "nommu: the record is still dropped");
    check(account_calls == 1 && last_freed_pages == 2, "nommu: pages are still accounted");
}

static int run_cache_puts, run_frame_puts;

void *mm_file_cache_mapping_get(void *vma, uintptr_t va, uint32_t pfn)
{
    (void)vma; (void)pfn;
    return (va == 0x1000 || va == 0x3000) ? (void *)0x9000 : NULL;
}
void page_cache_put(void *page)
{
    check(page == (void *)0x9000, "run release: cache object identity");
    run_cache_puts++;
}
void frame_put(uint32_t pfn)
{
    check(pfn == 20 || pfn == 40, "run release: only anonymous frames");
    run_frame_puts++;
}
extern void a20_vma_free_run_release_rust(void *vma, uintptr_t start_va,
                                          const uint32_t *pfns,
                                          void *const *owners, size_t count,
                                          int no_frame_ref);
static void run_release_mixed(void)
{
    uint32_t pfns[] = {10, 20, 30, 40};
    void *owners[] = {NULL, (void *)0x9000, NULL, NULL};
    a20_vma_free_run_release_rust(NULL, 0x1000, pfns, owners, 4, 0);
    check(run_cache_puts == 5,
          "run release: direct cache page puts once and fallback puts twice");
    check(run_frame_puts == 1, "run release: anonymous frames release once");
    run_cache_puts = run_frame_puts = 0;
    a20_vma_free_run_release_rust(NULL, 0x1000, pfns, owners, 4, 1);
    check(run_cache_puts == 5 && run_frame_puts == 0,
          "run release: no-frame-ref skips only frame release");
    run_cache_puts = run_frame_puts = 0;
    a20_vma_free_run_release_rust(NULL, 0x1000, pfns, NULL, 4, 0);
    check(run_cache_puts == 4 && run_frame_puts == 2,
          "run release: missing owner array preserves the old two-put fallback");
    run_cache_puts = run_frame_puts = 0;
}

int main(void)
{
    run_release_mixed();
    whole_record();
    partial_cuts();
    refusals_touch_nothing();
    release_classes();
    huge_leaf_is_demoted_first();
    never_faulted_reservation_is_retired();
    failure_paths();
    multi_record();
    nommu_releases_the_flat_allocation();
    reset();

    if (failures) {
        printf("MUNMAP_TXN_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MUNMAP_TXN_HOST: PASS %d checks\n", checks);
    return 0;
}
