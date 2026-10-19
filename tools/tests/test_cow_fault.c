/*
 * Host differential harness for kernel/mm/cow_fault.rs.
 *
 * The production Rust write-isolation transaction is linked against a model of
 * the mm contract.  Every assertion is about a decision the Rust module owns:
 * the leaf predicates, the canonical-cache-page copy (and that a shared VMA
 * never takes it), the refcount branch (0 panics, 1 upgrades in place, >1
 * copies), the install order, and the rollback of each failure.
 *
 * Guest coverage is separate: mm_stress's fork/COW and mseal phases run on real
 * page tables and audit the status sidecar.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define PMD_SIZE 0x200000UL
#define PMD_ORDER 9
#define PFN_NONE 0xFFFFFFFFu

#define VM_SHARED (1UL << 3)

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

struct a20_cowf_probe {
    void *slot;
    uintptr_t pte, base, size, pa;
    uint32_t pfn;
    int32_t present, is_leaf, user, cow, wr, pfn_valid, order;
};

/* ------------------------------------------------------------------ model */

static int g_pgdir = 1;
static struct a20_cowf_probe g_probe;
static uint64_t g_vm_flags;
static uint32_t g_alloc_page = 90;
static uint32_t g_alloc_order = 91;
static uint32_t g_refs = 1;
static int g_cache_hit;
static uint32_t g_cache_pfn;
static int g_install_rc_unused;

static int diag_begin_calls, alloc_page_calls, alloc_order_calls, copy_calls;
static int pfa_lock_calls, pfa_unlock_calls, zero_ref_calls, install_calls;
static int reuse_calls, write_d_calls, cache_put_calls;
static int last_install_tag, last_install_pfn;
static uintptr_t last_install_base;
static int last_copy_size;
static uint32_t last_old_pfn;
static int last_alloc_order;

static void reset(void)
{
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.pte = 0x100; /* opaque */
    g_probe.base = 0x2000;
    g_probe.size = PAGE_SIZE;
    g_probe.pa = 0x9000;
    g_probe.pfn = 7;
    g_probe.present = 1;
    g_probe.is_leaf = 1;
    g_probe.user = 1;
    g_probe.cow = 1;
    g_probe.wr = 0;
    g_probe.pfn_valid = 1;
    g_probe.order = 0;
    g_pgdir = 1;
    g_vm_flags = 0;
    g_alloc_page = 90;
    g_alloc_order = 91;
    g_refs = 2;
    g_cache_hit = 0;
    g_cache_pfn = 7;
    diag_begin_calls = alloc_page_calls = alloc_order_calls = copy_calls = 0;
    pfa_lock_calls = pfa_unlock_calls = zero_ref_calls = install_calls = 0;
    reuse_calls = write_d_calls = cache_put_calls = 0;
    last_install_tag = -1;
    last_install_pfn = 0;
    last_install_base = 0;
    last_copy_size = 0;
    last_old_pfn = 0;
    last_alloc_order = -1;
    (void)g_install_rc_unused;
}

/* ---------------------------------------------------------------- bridges */

int a20_cowf_has_pgdir(const void *mm) { (void)mm; return g_pgdir; }

void a20_cowf_probe(const void *mm, uintptr_t va, struct a20_cowf_probe *out)
{
    (void)mm;
    (void)va;
    *out = g_probe;
    out->slot = (void *)(uintptr_t)0x5107;
}

void *a20_cowf_vma(void *mm, uintptr_t va, uint64_t *vm_flags, int *present)
{
    (void)mm;
    (void)va;
    if (vm_flags)
        *vm_flags = g_vm_flags;
    if (present)
        *present = 1;
    return (void *)(uintptr_t)0x7A7A;
}

int a20_cowf_diag_begin(void *t, void *mm, uintptr_t va, uintptr_t base,
                        uintptr_t size)
{
    (void)t;
    (void)mm;
    (void)va;
    (void)base;
    (void)size;
    diag_begin_calls++;
    return 0;
}

uint32_t a20_cowf_alloc_page(void) { alloc_page_calls++; return g_alloc_page; }
uint32_t a20_cowf_alloc_order(int order)
{
    alloc_order_calls++;
    last_alloc_order = order;
    return g_alloc_order;
}
void a20_cowf_copy(uint32_t dst, uint32_t src, uintptr_t size)
{
    (void)dst;
    (void)src;
    copy_calls++;
    last_copy_size = (int)size;
}
uint64_t a20_cowf_pfa_lock(void) { pfa_lock_calls++; return 0xABCD; }
uint32_t a20_cowf_pfa_refs(uint32_t pfn) { (void)pfn; return g_refs; }
void a20_cowf_pfa_unlock(uint64_t flags) { (void)flags; pfa_unlock_calls++; }
void a20_cowf_zero_ref(void *t, uintptr_t va, uint32_t pfn, int order)
{
    (void)t;
    (void)va;
    (void)pfn;
    (void)order;
    zero_ref_calls++;
}

void a20_cowf_install(void *mm, uintptr_t va, uint32_t new_pfn,
                      uintptr_t snapshot_pte, uintptr_t base, uintptr_t size,
                      void *vma, void *slot, void *t, int diag, int tag,
                      int kind)
{
    (void)mm;
    (void)slot;
    (void)va;
    (void)snapshot_pte;
    (void)size;
    (void)vma;
    (void)t;
    (void)diag;
    (void)kind;
    install_calls++;
    last_install_tag = tag;
    last_install_pfn = (int)new_pfn;
    last_install_base = base;
}

void a20_cowf_reuse_install(void *mm, uintptr_t va, uintptr_t old_pa,
                            uintptr_t snapshot_pte, uintptr_t base,
                            uintptr_t size, void *vma, void *slot, void *t,
                            int diag, uint64_t pfa_flags)
{
    (void)mm;
    (void)slot;
    (void)va;
    (void)old_pa;
    (void)snapshot_pte;
    (void)size;
    (void)vma;
    (void)t;
    (void)diag;
    (void)pfa_flags;
    reuse_calls++;
    last_install_base = base;
    pfa_unlock_calls++; /* the bridge unlocks inside */
}

void a20_cowf_write_d(void *mm, uintptr_t va, uintptr_t snapshot_pte,
                      uintptr_t base, uintptr_t size, void *vma, void *slot)
{
    (void)mm;
    (void)slot;
    (void)va;
    (void)snapshot_pte;
    (void)size;
    (void)vma;
    write_d_calls++;
    last_install_base = base;
}

void *mm_file_cache_mapping_get(void *vma, uintptr_t va, uint32_t pfn)
{
    (void)vma;
    (void)va;
    if (g_cache_hit && pfn == g_cache_pfn)
        return (void *)(uintptr_t)0xC0FFEE;
    return NULL;
}

void page_cache_put(void *page)
{
    (void)page;
    cache_put_calls++;
}

extern int a20_cow_fault_locked_rust(void *t, void *mm, uintptr_t stval,
                                     uint32_t *old_pfn_out,
                                     void **old_page_out);

/* -------------------------------------------------------------- scenarios */

static void preconditions(void)
{
    static int dummy;
    uint32_t old_pfn = 123;
    void *old_page = (void *)1;

    reset();
    g_pgdir = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "no pgdir is -1");
    check(old_pfn == PFN_NONE && old_page == NULL,
          "the outputs are cleared before the refusal");

    reset();
    g_probe.present = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "an absent leaf is -1");
    reset();
    g_probe.is_leaf = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "a non-leaf is -1");
    reset();
    g_probe.user = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "a supervisor leaf is -1");
    reset();
    g_probe.cow = 0;
    g_probe.wr = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "neither COW nor writable is -1");
}

static void cache_page_copy(void)
{
    static int dummy;
    uint32_t old_pfn = 0;
    void *old_page = NULL;

    /* A private file leaf that is still the canonical cache frame is copied
     * out, and the cache reference is handed to the wrapper. */
    reset();
    g_cache_hit = 1;
    g_vm_flags = 0; /* private */
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "cache leaf: succeeds");
    check(alloc_page_calls == 1 && copy_calls == 1 && last_copy_size == (int)PAGE_SIZE,
          "cache leaf: one page is allocated and copied");
    check(install_calls == 1 && last_install_tag == 0,
          "cache leaf: the cache-copy install runs");
    check(old_page != NULL && old_pfn == PFN_NONE,
          "cache leaf: the cache reference goes to the wrapper");
    check(cache_put_calls == 0, "cache leaf: the wrapper owns the put");

    /* With no out-parameter the reference is dropped here. */
    reset();
    g_cache_hit = 1;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, NULL, NULL) == 0,
          "cache leaf no-out: succeeds");
    check(cache_put_calls == 1, "cache leaf no-out: the reference is dropped");

    /* A shared VMA never takes the private-copy branch. */
    reset();
    g_cache_hit = 1;
    g_vm_flags = VM_SHARED;
    g_refs = 2;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "shared leaf: succeeds");
    check(alloc_page_calls == 0, "shared leaf: no private page is allocated");
    check(cache_put_calls == 1, "shared leaf: the cache lookup is dropped");
    check(install_calls == 1 && last_install_tag == 1,
          "shared leaf: it falls through to the copy branch");

    /* A huge leaf never takes the cache branch even if the PFN matches. */
    reset();
    g_cache_hit = 1;
    g_probe.size = PMD_SIZE;
    g_probe.order = PMD_ORDER;
    g_refs = 2;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "huge leaf: succeeds");
    check(alloc_order_calls == 1 && last_alloc_order == PMD_ORDER,
          "huge leaf: it copies with the PMD order");
    check(last_copy_size == (int)PMD_SIZE, "huge leaf: the whole leaf is copied");
    check(old_pfn == g_probe.pfn, "huge leaf: the old frame is retired by the wrapper");
}

static void refcount_branches(void)
{
    static int dummy;
    uint32_t old_pfn = 0;
    void *old_page = NULL;

    /* A zero reference is a kernel invariant violation. */
    reset();
    g_refs = 0;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "zero ref: reported as failure");
    check(zero_ref_calls == 1, "zero ref: the invariant check ran");
    check(pfa_unlock_calls == 1, "zero ref: the allocator lock is released first");

    /* rc == 1 upgrades in place: same frame, no allocation, no copy. */
    reset();
    g_refs = 1;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "reuse: succeeds");
    check(reuse_calls == 1, "reuse: the in-place upgrade runs");
    check(alloc_page_calls == 0 && alloc_order_calls == 0 && copy_calls == 0,
          "reuse: nothing is allocated or copied");
    check(old_pfn == PFN_NONE, "reuse: no frame is retired");

    /* rc > 1 is a real copy. */
    reset();
    g_refs = 2;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "copy: succeeds");
    check(alloc_order_calls == 1 && last_alloc_order == 0 && copy_calls == 1,
          "copy: one base page is allocated and copied");
    check(install_calls == 1 && last_install_tag == 1 && last_install_pfn == 91,
          "copy: the fresh frame is installed");
    check(old_pfn == g_probe.pfn, "copy: the old frame is retired by the wrapper");
    check(pfa_unlock_calls == 1, "copy: the allocator lock is released before allocating");

    /* A failed allocation installs nothing. */
    reset();
    g_refs = 2;
    g_alloc_order = PFN_NONE;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == -1,
          "alloc failure: reported");
    check(install_calls == 0 && copy_calls == 0, "alloc failure: nothing is installed");
    check(old_pfn == PFN_NONE, "alloc failure: no frame is retired");
}

static void write_branch(void)
{
    static int dummy;
    uint32_t old_pfn = 0;
    void *old_page = NULL;

    reset();
    g_probe.cow = 0;
    g_probe.wr = 1;
    check(a20_cow_fault_locked_rust(&dummy, &dummy, 0x2000, &old_pfn, &old_page) == 0,
          "write: succeeds");
    check(write_d_calls == 1, "write: the dirty-bit branch runs");
    check(install_calls == 0 && alloc_order_calls == 0,
          "write: no install or allocation happens");
    check(last_install_base == g_probe.base, "write: the VMA base reaches the sync");
}

int main(void)
{
    preconditions();
    cache_page_copy();
    refcount_branches();
    write_branch();
    reset();

    if (failures) {
        printf("COW_FAULT_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("COW_FAULT_HOST: PASS %d checks\n", checks);
    return 0;
}
