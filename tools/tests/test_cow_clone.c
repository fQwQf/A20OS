/*
 * Host differential harness for kernel/mm/cow_clone.rs.
 *
 * The production Rust fork/COW clone transaction is linked against a model of
 * the mm contract.  Every assertion is about a decision the Rust module owns:
 * the DONTFORK/WIPEONFORK skip, the swap duplicate, the base-only rule, the
 * destination-already-present rule, the share-versus-private choice, the COW
 * flags and parent downgrade, and the rollback of each failure path.
 *
 * Guest coverage is separate: mm_stress's fork/COW phases run on real tables.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define PMD_SIZE 0x200000UL
#define PMD_ORDER 9

#define VM_DONTFORK (1UL << 9)
#define VM_WIPEONFORK (1UL << 10)

#define PFN_NONE 0xFFFFFFFFu

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

struct a20_cow_src {
    void *pcp;
    uintptr_t pte, base, size, pa;
    uintptr_t raw_flags, cow_flags;
    uint32_t pfn;
    int32_t level, present, swap, is_leaf, w_or_cow;
    uint32_t class;
    int32_t kind;
};

struct a20_cow_shared_snapshot {
    void *pcp, *slot, *node;
    uintptr_t pa, raw_flags, cow_flags;
    uint32_t pfn;
    int32_t w_or_cow;
    uint32_t class;
    int32_t kind;
};
_Static_assert(sizeof(struct a20_cow_shared_snapshot) == 64, "host snapshot ABI");
struct a20_cow_anon_batch_page {
    uint32_t pfn, class;
    uintptr_t flags;
    void *pcp;
    uint32_t kind;
};
static int snapshot_slot, snapshot_node, snapshot_locked;

/* ------------------------------------------------------------- the model */

static int g_policy_present;
static uint64_t g_policy_flags;
static int g_probe_present, g_probe_leaf, g_probe_swap, g_probe_level;
static uintptr_t g_probe_base, g_probe_size;
static uintptr_t g_probe_raw, g_probe_cow;
static int g_probe_w_or_cow;
static uint32_t g_probe_pfn;
static int g_dst_present;
static int g_swap_rc;
static int g_requires_copy;
static uint32_t g_alloc_pfn;
static int g_map_rc;
static int g_snapshot_ok;
static int g_snapshot_present, g_snapshot_leaf;
static int32_t g_snap_kind;

static int probe_calls, snapshot_calls, clone_swap_calls, dst_calls, alloc_calls, free_calls;
static int snapshot_at_calls;
static int copy_calls, map_calls, downgrade_calls, unlock_calls;
static int release_calls, release_kind, rss_calls, rss_pages;
static int merge_guard; /* unused; keeps the layout obvious */
static uintptr_t last_map_pfn;
static uintptr_t last_map_flags;
static int last_map_level;

static void reset(void)
{
    check(!snapshot_locked, "previous transaction released its node lock");
    g_policy_present = 1;
    g_policy_flags = 0;
    g_probe_present = 1;
    g_probe_leaf = 1;
    g_probe_swap = 0;
    g_probe_level = 0;
    g_probe_base = 0x1000;
    g_probe_size = PAGE_SIZE;
    g_probe_raw = 0x20;
    g_probe_cow = 0x120;
    g_probe_w_or_cow = 1;
    g_probe_pfn = 7;
    g_dst_present = 0;
    g_swap_rc = 0;
    g_requires_copy = 0;
    g_alloc_pfn = 42;
    g_map_rc = 0;
    g_snapshot_ok = 1;
    g_snapshot_present = g_snapshot_leaf = 1;
    g_snap_kind = 1;
    probe_calls = snapshot_calls = clone_swap_calls = dst_calls = alloc_calls = free_calls = 0;
    snapshot_at_calls = 0;
    copy_calls = map_calls = downgrade_calls = unlock_calls = 0;
    release_calls = rss_calls = 0;
    release_kind = -1;
    rss_pages = 0;
    last_map_pfn = 0;
    last_map_flags = 0;
    last_map_level = -1;
    (void)merge_guard;
}

uint64_t a20_cow_policy_flags(const void *parent, uintptr_t va, int *present)
{
    (void)parent;
    (void)va;
    *present = g_policy_present;
    return g_policy_flags;
}

void a20_cow_probe(const void *parent, uintptr_t va, struct a20_cow_src *out)
{
    (void)parent;
    (void)va;
    probe_calls++;
    memset(out, 0, sizeof(*out));
    out->pfn = PFN_NONE;
    out->present = g_probe_present;
    out->is_leaf = g_probe_leaf;
    out->swap = g_probe_swap;
    out->level = g_probe_level;
    out->base = g_probe_base;
    out->size = g_probe_size;
    out->raw_flags = g_probe_raw;
    out->cow_flags = g_probe_cow;
    out->w_or_cow = g_probe_w_or_cow;
    out->pfn = g_probe_pfn;
}

int a20_cow_dst_present(const void *child, uintptr_t va)
{
    (void)child;
    (void)va;
    dst_calls++;
    return g_dst_present;
}

int a20_cow_clone_swap(void *child, void *parent, uintptr_t va)
{
    (void)child;
    (void)parent;
    (void)va;
    clone_swap_calls++;
    return g_swap_rc;
}

void a20_cow_geometry(int level, uintptr_t *size, uintptr_t *pmd_size, int *pmd_order)
{
    if (size)
        *size = (level > 0) ? PMD_SIZE : PAGE_SIZE;
    if (pmd_size)
        *pmd_size = PMD_SIZE;
    if (pmd_order)
        *pmd_order = PMD_ORDER;
}

int a20_cow_requires_private_copy(void) { return g_requires_copy; }

uint32_t a20_cow_frame_alloc(int order)
{
    (void)order;
    check(snapshot_calls > release_calls && !snapshot_locked,
          "private allocation holds a source pin but no node lock");
    alloc_calls++;
    return g_alloc_pfn;
}

void a20_cow_frame_free(uint32_t pfn, int order)
{
    (void)pfn;
    (void)order;
    free_calls++;
}

void a20_cow_frame_copy(uint32_t dst, uint32_t src, uintptr_t size)
{
    (void)dst;
    (void)src;
    (void)size;
    copy_calls++;
}

int a20_cow_map_child(void *child, uintptr_t va, uint32_t pfn, uintptr_t flags,
                      uint32_t class, int level)
{
    (void)child;
    (void)va;
    (void)class;
    check(!snapshot_locked, "map child only after unlocking snapshot node");
    map_calls++;
    last_map_pfn = pfn;
    last_map_flags = flags;
    last_map_level = level;
    return g_map_rc;
}

/* The scalar suite does not enter batch mapping; provide its current ABI. */
int a20_cow_share_anon_batch_snapshot(void *parent, void *child,
                                      uintptr_t start, void *node,
                                      struct a20_cow_anon_batch_page *out)
{
    (void)parent; (void)child; (void)start; (void)node; (void)out;
    return 0;
}
int a20_cow_map_child_anon_batch(void *child, uintptr_t va,
                                 const struct a20_cow_anon_batch_page *pages,
                                 size_t count)
{
    (void)child; (void)va; (void)pages; (void)count;
    return -95;
}
void frame_put_many(const uint32_t *pfns, size_t count)
{
    (void)pfns; (void)count;
}
void page_cache_put(void *page)
{
    (void)page;
}

int a20_cow_share_snapshot(void *parent, uintptr_t va, int level,
                           struct a20_cow_shared_snapshot *out)
{
    (void)parent;
    (void)va;
    (void)level;
    snapshot_calls++;
    if (!g_snapshot_ok)
        return 0;
    snapshot_locked = 1;
    /* The real C helper validates present+leaf while holding the node lock. */
    if (!g_snapshot_present || !g_snapshot_leaf) {
        snapshot_locked = 0; /* the C helper releases its lock on rejection */
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->slot = &snapshot_slot;
    out->node = &snapshot_node;
    out->pfn = g_probe_pfn;
    out->pa = 0x9000;
    out->raw_flags = g_probe_raw;
    out->cow_flags = g_probe_cow;
    out->w_or_cow = g_probe_w_or_cow;
    out->class = 3;
    out->kind = g_snap_kind;
    out->pcp = (g_snap_kind == 2) ? (void *)(uintptr_t)0xC0FFEE : NULL;
    return 1;
}

int a20_cow_share_snapshot_at(void *parent, uintptr_t va, int level,
                              void *slot, void *node,
                              struct a20_cow_shared_snapshot *out)
{
    check(slot == &snapshot_slot && node == &snapshot_node,
          "snapshot-at receives the walker-borrowed slot and node");
    snapshot_at_calls++;
    return a20_cow_share_snapshot(parent, va, level, out);
}

void a20_cow_share_downgrade(void *parent, uintptr_t va, int level,
                             const struct a20_cow_shared_snapshot *snapshot,
                             uintptr_t flags)
{
    (void)parent; (void)va; (void)level;
    check(snapshot_locked && snapshot->slot == &snapshot_slot &&
          snapshot->node == &snapshot_node, "downgrade uses the pinned slot/node");
    check(snapshot->pa == 0x9000 && flags == g_probe_cow && snapshot->class == 3,
          "downgrade preserves snapshot physical address, flags and class");
    downgrade_calls++;
}

void a20_cow_share_unlock(const struct a20_cow_shared_snapshot *snapshot)
{
    check(snapshot_locked && snapshot->node == &snapshot_node,
          "unlock releases the exact snapshot node once");
    snapshot_locked = 0;
    unlock_calls++;
}

void a20_cow_release_ref(const struct a20_cow_shared_snapshot *src)
{
    check(!snapshot_locked, "failed child map releases reference after node unlock");
    release_calls++;
    release_kind = src ? src->kind : -1;
}

void a20_cow_rss_add(void *child, uintptr_t pages)
{
    (void)child;
    rss_calls++;
    rss_pages = (int)pages;
}

extern int a20_fork_clone_page_rust(void *child, void *parent, uintptr_t va,
                                    int shared);
extern int a20_cow_clone_leaf_rust(void *child, void *parent, uintptr_t va,
                                   int level, int shared);
extern int a20_cow_clone_leaf_at_rust(void *child, void *parent, uintptr_t va,
                                      int level, int shared, int checked,
                                      void *slot, void *node);

/* -------------------------------------------------------------- scenarios */

static void skip_and_dispatch(void)
{
    static int child, parent, mm;
    reset();
    g_policy_flags = VM_DONTFORK;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "dontfork: the page is skipped");
    check(probe_calls == 0, "dontfork: nothing is probed");
    g_policy_flags = VM_WIPEONFORK;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "wipeonfork: the page is skipped");

    reset();
    g_probe_present = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "absent: nothing to clone");
    check(map_calls == 0, "absent: nothing is mapped");

    reset();
    g_probe_leaf = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "not a leaf: nothing to clone");

    reset();
    g_probe_base = 0x800;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "non-base: a huge leaf is cloned only at its base");

    reset();
    g_dst_present = 1;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "dst present: the child leaf is left alone");
    check(map_calls == 0, "dst present: nothing is mapped");

    reset();
    g_probe_swap = 1;
    g_swap_rc = -17; /* EEXIST */
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "swap: EEXIST is tolerated");
    g_swap_rc = -11; /* EAGAIN */
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "swap: EAGAIN is tolerated");
    g_swap_rc = -12;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == -12,
          "swap: a real error is propagated");
    check(clone_swap_calls == 3, "swap: the swap path is taken every time");
    (void)mm;
}

static void private_copy(void)
{
    static int child, parent;
    reset();
    g_requires_copy = 1;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "private: the copy path succeeds");
    check(alloc_calls == 1 && copy_calls == 1 && map_calls == 1,
          "private: allocate, copy, map");
    check(last_map_pfn == 42, "private: the child maps the fresh frame");
    check(last_map_flags == g_probe_raw, "private: the child keeps the raw arch flags");
    check(rss_calls == 1 && rss_pages == 1, "private: rss is charged");
    check(downgrade_calls == 0, "private: the parent is not downgraded");
    check(unlock_calls == 1 && release_calls == 1,
          "private: source pin is released after the map");

    /* A huge leaf copies with the PMD order. */
    reset();
    g_requires_copy = 1;
    g_probe_level = 1;
    g_probe_base = 0x1000;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "private huge: succeeds");
    check(last_map_level == 1, "private huge: the child map is huge");
    check(rss_calls == 1 && rss_pages == (int)(PMD_SIZE / PAGE_SIZE),
          "private huge: rss counts the whole leaf");

    /* A failed child map frees the copy. */
    reset();
    g_requires_copy = 1;
    g_map_rc = -12;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == -12,
          "private: a failed map is propagated");
    check(free_calls == 1, "private: the copied frame is freed");
    check(rss_calls == 0, "private: a failed copy charges no rss");
    check(release_calls == 1, "private: map failure releases source pin");

    /* A failed frame allocation maps nothing. */
    reset();
    g_requires_copy = 1;
    g_alloc_pfn = PFN_NONE;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == -12,
          "private: an allocation failure is ENOMEM");
    check(copy_calls == 0 && map_calls == 0, "private: nothing is copied or mapped");
    check(release_calls == 1, "private: allocation failure releases source pin");
}

static void share_and_cow(void)
{
    static int child, parent;
    /* Private: share, downgrade the parent, map the child with COW flags. */
    reset();
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "share: succeeds");
    check(downgrade_calls == 1, "share: the writable parent is downgraded");
    check(unlock_calls == 1, "share: the node lock is released");
    check(last_map_flags == g_probe_cow, "share: the child gets the COW flags");
    check(last_map_pfn == g_probe_pfn, "share: the child maps the same frame");
    check(release_calls == 0, "share: no reference is dropped on success");
    check(probe_calls == 1, "address entry: only its dispatch probe runs for sharing");

    /* A read-only leaf needs no downgrade but is still shared. */
    reset();
    g_probe_w_or_cow = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "share ro: succeeds");
    check(downgrade_calls == 0, "share ro: no downgrade is needed");
    check(map_calls == 1, "share ro: the child is still mapped");

    /* MAP_SHARED keeps the raw arch flags. */
    reset();
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 1) == 0,
          "shared: succeeds");
    check(downgrade_calls == 0, "shared: the parent is never downgraded");
    check(last_map_flags == g_probe_raw, "shared: the child gets the raw flags");

    /* The C walker already supplies a leaf: sharing uses only its snapshot. */
    reset();
    check(a20_cow_clone_leaf_rust(&child, &parent, 0x1000, 0, 0) == 0,
          "direct leaf: sharing succeeds");
    check(probe_calls == 0 && snapshot_calls == 1,
          "direct leaf: shared path skips the unlocked probe");

    reset();
    check(a20_cow_clone_leaf_at_rust(&child, &parent, 0x1000, 0, 0, 0,
                                     &snapshot_slot, &snapshot_node) == 0,
          "walker hint: shared leaf succeeds");
    check(probe_calls == 0 && snapshot_at_calls == 1 && snapshot_calls == 1,
          "walker hint: uses supplied slot without an unlocked probe");

    /* The locked snapshot independently rejects a source that vanished or
       ceased to be a leaf after the dispatch probe. */
    reset();
    g_snapshot_present = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "snapshot absent: source disappearance is tolerated");
    check(map_calls == 0 && unlock_calls == 0 && rss_calls == 0 && !snapshot_locked,
          "snapshot absent: no map, unlock, or rss charge");
    reset();
    g_snapshot_leaf = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "snapshot not leaf: source change is tolerated");
    check(map_calls == 0 && unlock_calls == 0 && rss_calls == 0 && !snapshot_locked,
          "snapshot not leaf: no map, unlock, or rss charge");

    reset();
    g_policy_flags = VM_DONTFORK;
    check(a20_cow_clone_leaf_at_rust(&child, &parent, 0x1000, 0, 0, 1,
                                     &snapshot_slot, &snapshot_node) == 0,
          "walker checked: dontfork is skipped");
    check(snapshot_at_calls == 0 && map_calls == 0,
          "walker checked: policy skip precedes snapshot");
    reset();
    g_dst_present = 1;
    check(a20_cow_clone_leaf_at_rust(&child, &parent, 0x1000, 0, 0, 1,
                                     &snapshot_slot, &snapshot_node) == 0,
          "walker checked: existing child leaf is skipped");
    check(snapshot_at_calls == 0 && map_calls == 0,
          "walker checked: destination check precedes snapshot");

    /* A failed child map gives back exactly the reference that was taken. */
    reset();
    g_map_rc = -12;
    g_snap_kind = 1;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == -12,
          "share fail: the error is propagated");
    check(release_calls == 1 && release_kind == 1,
          "share fail: the frame reference is dropped");

    reset();
    g_map_rc = -12;
    g_snap_kind = 2;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == -12,
          "share fail cache: the error is propagated");
    check(release_calls == 1 && release_kind == 2,
          "share fail cache: the page-cache reference is dropped");

    /* A leaf that vanished under the lock is not an error. */
    reset();
    g_snapshot_ok = 0;
    check(a20_fork_clone_page_rust(&child, &parent, 0x1000, 0) == 0,
          "vanished: the leaf disappearing under the lock is not an error");
    check(map_calls == 0 && unlock_calls == 0,
          "vanished: nothing is mapped and no lock is held");
}

static void private_probe_validation(void)
{
    static int child, parent;
    reset();
    g_requires_copy = 1;
    g_snapshot_present = 0;
    check(a20_cow_clone_leaf_rust(&child, &parent, 0x1000, 0, 0) == 0,
          "private direct leaf: absent source is skipped");
    check(snapshot_calls == 1 && alloc_calls == 0 && map_calls == 0,
          "private direct leaf: snapshot validates before allocation");
    reset();
    g_requires_copy = 1;
    g_snapshot_leaf = 0;
    check(a20_cow_clone_leaf_rust(&child, &parent, 0x1000, 0, 0) == 0,
          "private direct leaf: non-leaf source is skipped");
    check(alloc_calls == 0 && map_calls == 0,
          "private direct leaf: non-leaf is not copied");

    reset();
    g_requires_copy = 1;
    check(a20_cow_clone_leaf_at_rust(&child, &parent, 0x1000, 0, 0, 0,
                                     &snapshot_slot, &snapshot_node) == 0,
          "walker hint: private-copy succeeds");
    check(probe_calls == 0 && snapshot_at_calls == 1 && map_calls == 1,
          "walker hint: private-copy pins the walker source before allocation");
}

int main(void)
{
    skip_and_dispatch();
    private_copy();
    share_and_cow();
    private_probe_validation();
    reset();

    if (failures) {
        printf("COW_CLONE_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("COW_CLONE_HOST: PASS %d checks\n", checks);
    return 0;
}
