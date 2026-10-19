/*
 * Host differential and failure-injection harness for kernel/mm/mmap_policy.rs.
 *
 * The production Rust module is linked against these stubs, which model just
 * enough of the kernel's mm contract to drive every fallible step of the
 * mapping-creation transaction: record allocation, the fixed/hint address
 * contracts, W^X refusal, the page-table reservation, the mlock admission, and
 * the per-backing reference acquisitions.  Every assertion below is about the
 * ORDER and OWNERSHIP the Rust transaction decides -- which step ran, what was
 * rolled back when it failed, and what was never charged or published.
 *
 * This is a state-machine oracle, not a kernel.  It does not establish real
 * VMA allocator, page-table, or guest behavior; the guest cases do that.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define USER_VA_LIMIT 0x4000000000UL
#define MMAP_BASE_ADDR 0x60000000UL

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_HUGETLB 0x40000
#define MAP_FIXED_NOREPLACE 0x100000

#define VM_READ (1UL << 0)
#define VM_WRITE (1UL << 1)
#define VM_EXEC (1UL << 2)
#define VM_SHARED (1UL << 3)
#define VM_ANON (1UL << 4)
#define VM_HUGEPAGE (1UL << 11)
#define VM_FILE (1UL << 13)
#define VM_VMO (1UL << 14)
#define VM_LOCKED (1UL << 15)
#define VM_NATIVE_CAP (1UL << 19)

#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17
#define EACCES 13
#define EOPNOTSUPP 95

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

/* Error encoding: the kernel returns `(vaddr_t)-errno`. */
static int result_errno(unsigned long r)
{
    return (int)(long)r;
}

static int result_is_error(unsigned long r)
{
    return (long)r < 0 && (long)r > -4096;
}

struct stub_seg {
    unsigned long start, end, vm_flags, pte_flags;
    unsigned vmar_cap;
    void *file;
    unsigned long backing_offset;
    void *file_vnode;
    void *vmo;
    void *vmar_owner;
    void *nommu_alloc;
    struct stub_seg *next;
};

struct stub_mm {
    struct stub_seg *mmap;
    unsigned long mmap_base;
    unsigned long total_vm;
    unsigned long locked_vm;
    unsigned def_locked;
    unsigned long memlock;
    int has_cap;
};

static struct stub_mm g_mm;

static int fail_seg_alloc;
static int munmap_fail;
static int wx_deny;
static int provision_count;
static int munmap_count;
static int insert_count;
static int vnode_get_count;
static int vnode_put_count;
static int shared_inc_count;
static int vmar_acquire_count;
static int vmo_ref_count;
static int free_count;
static int alloc_count;
static int gap_count;
static int is_nommu;
static int nommu_fail;
static int nommu_alloc_count;
static char last_wx_ctx[32];

static void reset_state(void)
{
    struct stub_seg *s = g_mm.mmap;
    while (s) {
        struct stub_seg *n = s->next;
        free(s);
        s = n;
    }
    memset(&g_mm, 0, sizeof(g_mm));
    g_mm.mmap_base = MMAP_BASE_ADDR;
    g_mm.memlock = 1UL << 20;
    g_mm.has_cap = 0;
    fail_seg_alloc = 0;
    munmap_fail = 0;
    wx_deny = 0;
    provision_count = munmap_count = insert_count = 0;
    vnode_get_count = vnode_put_count = shared_inc_count = 0;
    vmar_acquire_count = vmo_ref_count = 0;
    free_count = alloc_count = gap_count = 0;
    is_nommu = 0;
    nommu_fail = 0;
    nommu_alloc_count = 0;
    last_wx_ctx[0] = '\0';
}

/* ---------------------------------------------------------------- stubs -- */

void *mm_seg_new(void)
{
    if (fail_seg_alloc)
        return NULL;
    struct stub_seg *s = calloc(1, sizeof(*s));
    if (s)
        alloc_count++;
    return s;
}

void kfree(void *p)
{
    if (p) {
        free_count++;
        free(p);
    }
}

void mm_insert_vma(void *mm_ptr, void *vma_ptr)
{
    struct stub_mm *mm = mm_ptr;
    struct stub_seg *v = vma_ptr;
    struct stub_seg **pp = &mm->mmap;
    insert_count++;
    while (*pp && (*pp)->start < v->start)
        pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}

unsigned long mm_find_gap(void *mm_ptr, unsigned long hint, unsigned long len)
{
    struct stub_mm *mm = mm_ptr;
    unsigned long cursor = hint;
    gap_count++;
    for (struct stub_seg *v = mm->mmap; v; v = v->next) {
        if (v->start >= cursor && v->start - cursor >= len)
            return cursor;
        if (v->end > cursor)
            cursor = v->end;
    }
    return cursor;
}

int mm_range_overlaps(void *mm_ptr, unsigned long start, unsigned long len,
                      const void *ignore)
{
    struct stub_mm *mm = mm_ptr;
    unsigned long end = start + len;
    if (end < start)
        return 1;
    for (struct stub_seg *v = mm->mmap; v; v = v->next) {
        if (v != ignore && v->start < end && v->end > start)
            return 1;
    }
    return 0;
}

int mm_munmap_locked(void *mm_ptr, unsigned long addr, unsigned long len)
{
    struct stub_mm *mm = mm_ptr;
    unsigned long end = addr + len;
    struct stub_seg **pp = &mm->mmap;
    munmap_count++;
    if (munmap_fail)
        return -ENOMEM;
    while (*pp) {
        struct stub_seg *v = *pp;
        if (v->end <= addr || v->start >= end) {
            pp = &v->next;
            continue;
        }
        *pp = v->next;
        free(v);
    }
    return 0;
}

int mm_wx_filter_prot(int prot, const char *ctx)
{
    snprintf(last_wx_ctx, sizeof(last_wx_ctx), "%s", ctx ? ctx : "?");
    if (wx_deny && (prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
        return -EACCES;
    return prot;
}

unsigned long mm_prot_to_pte_flags(int prot)
{
    return (unsigned long)prot;
}

int mm_pte_flags_to_prot(unsigned long pte_flags)
{
    return (int)pte_flags;
}

void vnode_get(void *vn) { (void)vn; vnode_get_count++; }
void vnode_put(void *vn) { (void)vn; vnode_put_count++; }
void vmar_acquire(void *vmar) { (void)vmar; vmar_acquire_count++; }
void vmo_ref(void *vmo) { (void)vmo; vmo_ref_count++; }

/* ------------------------------------------------------- Rust bridges ---- */

struct a20_mmap_seg_spec {
    uintptr_t start;
    uintptr_t end;
    uint64_t vm_flags;
    uintptr_t pte_flags;
    uint32_t vmar_cap;
    uint32_t reserved;
    void *file;
    uint64_t backing_offset;
    void *file_vnode;
    void *vmo;
    void *vmar_owner;
};

void a20_mmap_seg_fill(void *vma_ptr, const struct a20_mmap_seg_spec *spec,
                       void *nommu_raw)
{
    struct stub_seg *v = vma_ptr;
    v->nommu_alloc = nommu_raw;
    v->start = spec->start;
    v->end = spec->end;
    v->vm_flags = spec->vm_flags;
    v->pte_flags = spec->pte_flags;
    v->vmar_cap = spec->vmar_cap;
    v->file = spec->file;
    v->backing_offset = spec->backing_offset;
    v->file_vnode = spec->file_vnode;
    v->vmo = spec->vmo;
    v->vmar_owner = spec->vmar_owner;
}

uintptr_t a20_mmap_mmap_base(const void *mm) { return ((const struct stub_mm *)mm)->mmap_base; }
int a20_mmap_def_locked(const void *mm) { return ((const struct stub_mm *)mm)->def_locked != 0; }
uintptr_t a20_mmap_locked_vm(const void *mm) { return ((const struct stub_mm *)mm)->locked_vm; }
void a20_mmap_locked_vm_add(void *mm, uintptr_t len) { ((struct stub_mm *)mm)->locked_vm += len; }
void a20_mmap_total_vm_add(void *mm, uintptr_t pages) { ((struct stub_mm *)mm)->total_vm += pages; }
uint64_t a20_mmap_memlock_limit(void) { return g_mm.memlock; }
int a20_mmap_has_cap_admin(void) { return g_mm.has_cap; }
void *a20_mmap_file_vnode(void *file) { return file; }   /* the "vfile" IS the vnode */
uint64_t a20_mmap_vmo_size(void *vmo) { return *(const uint64_t *)vmo; }
void a20_mmap_vnode_shared_map_inc(void *vn) { (void)vn; shared_inc_count++; }
void a20_mmap_provision_anon(void *mm, uintptr_t s, uintptr_t e, uintptr_t f)
{ (void)mm; (void)s; (void)e; (void)f; provision_count++; }
uintptr_t a20_mmap_user_va_limit(void) { return USER_VA_LIMIT; }
uintptr_t a20_mmap_base_addr(void) { return MMAP_BASE_ADDR; }
int a20_mmap_is_nommu(void) { return is_nommu; }

/* Models the kernel's flat allocation: a raw kmalloc-backed block plus the
 * page-aligned address inside it the record will name.  The stub never writes
 * through the address; the transaction's ownership of the raw pointer is what
 * is under test. */
void *a20_mmap_nommu_alloc(uintptr_t len, uintptr_t *addr_out)
{
    nommu_alloc_count++;
    if (nommu_fail)
        return NULL;
    void *raw = malloc(len + PAGE_SIZE);
    if (!raw)
        return NULL;
    if (addr_out)
        *addr_out = 0x80000000UL + (uintptr_t)nommu_alloc_count * PAGE_SIZE;
    return raw;
}

/* -------------------------------------------------- Rust entry points ---- */

extern unsigned long a20_mmap_anon_rust(void *mm, unsigned long addr,
                                        unsigned long len, int prot, int flags,
                                        int native_cap);
extern unsigned long a20_mmap_file_rust(void *mm, unsigned long addr,
                                        unsigned long len, int prot, int flags,
                                        void *file, uint64_t file_offset,
                                        int native_cap, void *owner);
extern unsigned long a20_mmap_vmo_rust(void *mm, unsigned long addr,
                                       unsigned long len, int prot, int flags,
                                       void *vmo, uint64_t vmo_offset,
                                       int native_cap, void *owner);

/* ----------------------------------------------------------- scenarios --- */

static struct stub_seg *only_seg(void)
{
    return g_mm.mmap;
}

static void anon_publication_and_accounting(void)
{
    reset_state();
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, 3 * PAGE_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "anon: mapping succeeds");
    check(r == MMAP_BASE_ADDR, "anon: gap search starts at mmap_base");
    struct stub_seg *v = only_seg();
    check(v && v->start == MMAP_BASE_ADDR && v->end == MMAP_BASE_ADDR + 3 * PAGE_SIZE,
          "anon: extent is the rounded request");
    check(v && (v->vm_flags & (VM_ANON | VM_READ | VM_WRITE)) ==
                   (VM_ANON | VM_READ | VM_WRITE),
          "anon: vm_flags carry the anon class and the requested access");
    check(v && v->vmar_cap == (unsigned)(PROT_READ | PROT_WRITE),
          "anon: vmar_cap is the protection ceiling");
    check(g_mm.total_vm == 3, "anon: total_vm charged once per page");
    check(provision_count == 1, "anon: private anon reserves per-PTE status");
    check(insert_count == 1, "anon: record published exactly once");
    check(last_wx_ctx[0] && strcmp(last_wx_ctx, "mmap") == 0,
          "anon: W^X decision sees the mmap context");
}

static void anon_len_rounds_and_rejects(void)
{
    reset_state();
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, 1, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "anon: sub-page length accepted");
    check(g_mm.total_vm == 1, "anon: sub-page length rounds up to one page");

    reset_state();
    r = a20_mmap_anon_rust(&g_mm, 0, USER_VA_LIMIT + PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "anon: over-limit length is ENOMEM");
    check(alloc_count == 0, "anon: over-limit length allocates nothing");

    reset_state();
    r = a20_mmap_anon_rust(&g_mm, 0, 0, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "anon: zero length is EINVAL");
}

static void anon_fixed_contracts(void)
{
    /* MAP_FIXED_NOREPLACE on a free range upgrades to MAP_FIXED. */
    reset_state();
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0x20000000, PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS |
                                             MAP_FIXED_NOREPLACE, -1);
    check(r == 0x20000000, "fixed-noreplace: free range is honoured");
    /* Promotion to MAP_FIXED means the free range is still passed through the
     * ordinary unmap transaction, exactly as the C reference did; it is a
     * no-op there, not a skipped step. */
    check(munmap_count == 1,
          "fixed-noreplace: a free range is promoted through the ordinary unmap");

    /* Occupied range answers EEXIST and never reaches munmap. */
    reset_state();
    a20_mmap_anon_rust(&g_mm, 0x20000000, 4 * PAGE_SIZE, PROT_READ,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    int before_munmap = munmap_count;
    r = a20_mmap_anon_rust(&g_mm, 0x20000000, PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1);
    check(result_is_error(r) && result_errno(r) == -EEXIST,
          "fixed-noreplace: occupied range is EEXIST");
    check(munmap_count == before_munmap,
          "fixed-noreplace: refusal never releases the occupant");

    /* MAP_FIXED releases the occupant first, and a release failure is fatal. */
    reset_state();
    a20_mmap_anon_rust(&g_mm, 0x20000000, PAGE_SIZE, PROT_READ,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    int allocs_before = alloc_count;
    munmap_fail = 1;
    r = a20_mmap_anon_rust(&g_mm, 0x20000000, PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "fixed: a failed replacement propagates the unmap error");
    check(alloc_count == allocs_before,
          "fixed: no record is allocated when the replacement fails");
    check(g_mm.locked_vm == 0 && g_mm.total_vm == 1,
          "fixed: a failed replacement charges nothing");

    /* A misaligned MAP_FIXED address is EINVAL before anything else runs. */
    reset_state();
    r = a20_mmap_anon_rust(&g_mm, 0x20000001, PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "fixed: misaligned fixed address is EINVAL");
    check(munmap_count == 0, "fixed: misalignment is checked before any release");
}

static void anon_hint_is_dropped_not_failed(void)
{
    /* An out-of-range hint falls back to the gap search. */
    reset_state();
    unsigned long r = a20_mmap_anon_rust(&g_mm, USER_VA_LIMIT + PAGE_SIZE,
                                         PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "hint: out-of-range hint is dropped, not failed");
    check(r == MMAP_BASE_ADDR, "hint: dropped hint falls back to the gap search");

    /* A hint that collides with an existing mapping is dropped too. */
    reset_state();
    a20_mmap_anon_rust(&g_mm, MMAP_BASE_ADDR, PAGE_SIZE, PROT_READ,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    r = a20_mmap_anon_rust(&g_mm, MMAP_BASE_ADDR, PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "hint: colliding hint is dropped, not failed");
    check(r == MMAP_BASE_ADDR + PAGE_SIZE, "hint: collision falls to the next gap");
}

static void anon_wx_refusal_allocates_nothing(void)
{
    reset_state();
    wx_deny = 1;
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, PAGE_SIZE,
                                         PROT_READ | PROT_WRITE | PROT_EXEC,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -EACCES,
          "wx: a refused W|X mapping is EACCES");
    check(alloc_count == 0, "wx: refusal happens before the record is allocated");
    check(insert_count == 0, "wx: refusal publishes nothing");
}

static void anon_mlock_admission(void)
{
    /* Over the ceiling without CAP_SYS_ADMIN: refused, nothing charged. */
    reset_state();
    g_mm.def_locked = 1;
    g_mm.memlock = PAGE_SIZE;
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, 2 * PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "mlock: over-limit mapping is ENOMEM");
    check(g_mm.locked_vm == 0, "mlock: refused mapping charges no locked_vm");
    check(free_count == 1, "mlock: refused mapping frees its record");
    check(insert_count == 0, "mlock: refused mapping is never published");

    /* The capability bypasses the ceiling, and the charge is recorded. */
    reset_state();
    g_mm.def_locked = 1;
    g_mm.memlock = PAGE_SIZE;
    g_mm.has_cap = 1;
    r = a20_mmap_anon_rust(&g_mm, 0, 2 * PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "mlock: CAP_SYS_ADMIN admits the mapping");
    check(g_mm.locked_vm == 2 * PAGE_SIZE, "mlock: admitted mapping charges locked_vm");
    check((only_seg()->vm_flags & VM_LOCKED) != 0,
          "mlock: admitted mapping carries VM_LOCKED");
}

static void anon_record_alloc_failure(void)
{
    reset_state();
    fail_seg_alloc = 1;
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "alloc: a failed record allocation is ENOMEM");
    check(g_mm.total_vm == 0 && g_mm.locked_vm == 0,
          "alloc: a failed record allocation charges nothing");
    check(insert_count == 0, "alloc: a failed record allocation publishes nothing");
}

static void anon_native_cap_zero_ceiling(void)
{
    reset_state();
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, PAGE_SIZE, PROT_NONE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, 0);
    check(!result_is_error(r), "native: PROT_NONE native mapping succeeds");
    struct stub_seg *v = only_seg();
    check((v->vm_flags & VM_NATIVE_CAP) != 0,
          "native: a zero ceiling is still marked as a capability mapping");
    check(v->vmar_cap == 0, "native: vmar_cap is the caller's ceiling");
}

static void file_ownership_and_rollback(void)
{
    static int fake_vnode;
    void *file = &fake_vnode;

    /* Success: the record takes the caller's file reference and retains the
     * vnode.  A plain MAP_PRIVATE mapping does not count as a shared map. */
    reset_state();
    unsigned long r = a20_mmap_file_rust(&g_mm, 0, 2 * PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE, file, 0, -1, NULL);
    check(!result_is_error(r), "file: mapping succeeds");
    struct stub_seg *v = only_seg();
    check(v && v->file == file, "file: the record adopts the caller's reference");
    check(v && v->file_vnode == file, "file: the record retains its vnode");
    check(vnode_get_count == 1, "file: the vnode is retained exactly once");
    check(shared_inc_count == 0, "file: private mapping is not a shared map");
    check(vmar_acquire_count == 0, "file: no owner means no vmar reference");
    check((v->vm_flags & VM_FILE) != 0 && (v->vm_flags & VM_SHARED) == 0,
          "file: vm_flags carry the file class without sharing");

    /* MAP_SHARED counts the vnode as a shared mapping and acquires the owner. */
    static int fake_vmar;
    reset_state();
    r = a20_mmap_file_rust(&g_mm, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, file, 0, -1, &fake_vmar);
    check(!result_is_error(r), "file: shared mapping succeeds");
    check(shared_inc_count == 1, "file: shared mapping counts the vnode");
    check(vmar_acquire_count == 1, "file: the placement owner is acquired");

    /* A refused mlock admission drops the vnode reference it took and frees
     * the record, and never puts the caller's file reference. */
    reset_state();
    g_mm.def_locked = 1;
    g_mm.memlock = PAGE_SIZE;
    r = a20_mmap_file_rust(&g_mm, 0, 4 * PAGE_SIZE, PROT_READ, MAP_PRIVATE,
                           file, 0, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "file: refused mlock admission is ENOMEM");
    check(vnode_get_count == 1 && vnode_put_count == 1,
          "file: the refused mapping takes and drops exactly one vnode reference");
    check(free_count == 1, "file: the refused mapping frees its record");
    check(shared_inc_count == 0, "file: the refused mapping never counts as shared");
    check(insert_count == 0, "file: the refused mapping is never published");

    /* Argument validation happens before any allocation or reference. */
    reset_state();
    r = a20_mmap_file_rust(&g_mm, 0, PAGE_SIZE, PROT_READ, MAP_PRIVATE,
                           file, 1, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "file: a misaligned file offset is EINVAL");
    r = a20_mmap_file_rust(&g_mm, 0, PAGE_SIZE, PROT_READ, MAP_PRIVATE,
                           NULL, 0, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "file: a missing file is EINVAL");
    check(vnode_get_count == 0 && alloc_count == 0,
          "file: validation precedes allocation and reference acquisition");
}

static void vmo_bounds_and_reference(void)
{
    static uint64_t fake_vmo_size = 8 * PAGE_SIZE;
    void *vmo = &fake_vmo_size;

    reset_state();
    unsigned long r = a20_mmap_vmo_rust(&g_mm, 0, 2 * PAGE_SIZE, PROT_READ,
                                        MAP_SHARED, vmo, PAGE_SIZE, -1, NULL);
    check(!result_is_error(r), "vmo: mapping succeeds");
    struct stub_seg *v = only_seg();
    check(v && v->vmo == vmo, "vmo: the record names the VMO");
    check(v && v->backing_offset == PAGE_SIZE, "vmo: the record carries the offset");
    check((v->vm_flags & VM_VMO) != 0, "vmo: vm_flags carry the VMO class");
    check(vmo_ref_count == 1, "vmo: the VMO is referenced exactly once");
    check(g_mm.locked_vm == 0,
          "vmo: no mlock admission block, matching the existing contract");

    reset_state();
    r = a20_mmap_vmo_rust(&g_mm, 0, 2 * PAGE_SIZE, PROT_READ, MAP_SHARED,
                          vmo, fake_vmo_size, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "vmo: an offset at the end of the object is EINVAL");
    check(vmo_ref_count == 0, "vmo: a rejected range takes no VMO reference");

    reset_state();
    r = a20_mmap_vmo_rust(&g_mm, 0, fake_vmo_size + PAGE_SIZE, PROT_READ,
                          MAP_SHARED, vmo, 0, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "vmo: a range past the end of the object is EINVAL");

    reset_state();
    r = a20_mmap_vmo_rust(&g_mm, 0, PAGE_SIZE, PROT_READ, MAP_SHARED,
                          vmo, 1, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EINVAL,
          "vmo: a misaligned offset is EINVAL");
    check(alloc_count == 0, "vmo: bounds are checked before allocation");
}

static void nommu_flat_allocation_ignores_the_request(void)
{
    /* NOMMU has no address space: both a zero address and an explicit
     * MAP_FIXED address must end up at freshly allocated flat memory, and the
     * gap search must not run at all. */
    reset_state();
    is_nommu = 1;
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, 2 * PAGE_SIZE,
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "nommu: anonymous mapping succeeds");
    check(r == 0x80000000UL + PAGE_SIZE, "nommu: address is the flat allocation");
    check(gap_count == 0, "nommu: the gap search is never consulted");
    struct stub_seg *v = only_seg();
    check(v && v->nommu_alloc != NULL, "nommu: the record owns the flat block");
    check(v && v->start == r && v->end == r + 2 * PAGE_SIZE,
          "nommu: extent is the flat block");
    check(g_mm.total_vm == 2, "nommu: total_vm is charged normally");

    reset_state();
    is_nommu = 1;
    r = a20_mmap_anon_rust(&g_mm, MMAP_BASE_ADDR, PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1);
    check(!result_is_error(r), "nommu: MAP_FIXED mapping succeeds");
    check(r != MMAP_BASE_ADDR, "nommu: MAP_FIXED does not alias the named address");
    check(gap_count == 0, "nommu: MAP_FIXED does not fall back to the gap search");

    /* The hint is dropped rather than failed, exactly as on the MMU path. */
    reset_state();
    is_nommu = 1;
    r = a20_mmap_anon_rust(&g_mm, USER_VA_LIMIT + PAGE_SIZE, PAGE_SIZE,
                           PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(!result_is_error(r), "nommu: an out-of-range hint is dropped, not failed");
}

static void nommu_allocation_failure_and_rollback(void)
{
    reset_state();
    is_nommu = 1;
    nommu_fail = 1;
    unsigned long r = a20_mmap_anon_rust(&g_mm, 0, PAGE_SIZE, PROT_READ,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "nommu: a failed flat allocation is ENOMEM");
    check(alloc_count == 0, "nommu: a failed flat allocation allocates no record");
    check(insert_count == 0, "nommu: a failed flat allocation publishes nothing");

    /* A refused mlock admission must release BOTH the record and the flat
     * block; the record free alone would leak the block. */
    reset_state();
    is_nommu = 1;
    g_mm.def_locked = 1;
    g_mm.memlock = PAGE_SIZE;
    r = a20_mmap_anon_rust(&g_mm, 0, 4 * PAGE_SIZE, PROT_READ,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "nommu: refused mlock admission is ENOMEM");
    check(nommu_alloc_count == 1, "nommu: the refused mapping did allocate flat memory");
    check(free_count == 2,
          "nommu: the refused mapping frees its record and its flat block");
    check(g_mm.locked_vm == 0, "nommu: the refused mapping charges no locked_vm");

    /* The file path rolls back the same way, plus its vnode reference. */
    static int fake_vnode;
    reset_state();
    is_nommu = 1;
    g_mm.def_locked = 1;
    g_mm.memlock = PAGE_SIZE;
    r = a20_mmap_file_rust(&g_mm, 0, 4 * PAGE_SIZE, PROT_READ, MAP_PRIVATE,
                           &fake_vnode, 0, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -ENOMEM,
          "nommu: refused file mapping is ENOMEM");
    check(vnode_get_count == 1 && vnode_put_count == 1,
          "nommu: the refused file mapping drops its vnode reference");
    check(free_count == 2,
          "nommu: the refused file mapping frees its record and its flat block");
}

static void nommu_vmo_is_unsupported(void)
{
    static uint64_t fake_vmo_size = 8 * PAGE_SIZE;
    void *vmo = &fake_vmo_size;

    reset_state();
    is_nommu = 1;
    unsigned long r = a20_mmap_vmo_rust(&g_mm, 0, PAGE_SIZE, PROT_READ,
                                        MAP_SHARED, vmo, 0, -1, NULL);
    check(result_is_error(r) && result_errno(r) == -EOPNOTSUPP,
          "nommu: a VMO mapping is EOPNOTSUPP");
    check(vmo_ref_count == 0, "nommu: the refused VMO mapping takes no reference");
    check(nommu_alloc_count == 0, "nommu: the refused VMO mapping allocates nothing");
    check(alloc_count == 0, "nommu: the refused VMO mapping creates no record");
}

int main(void)
{
    anon_publication_and_accounting();
    anon_len_rounds_and_rejects();
    anon_fixed_contracts();
    anon_hint_is_dropped_not_failed();
    anon_wx_refusal_allocates_nothing();
    anon_mlock_admission();
    anon_record_alloc_failure();
    anon_native_cap_zero_ceiling();
    file_ownership_and_rollback();
    vmo_bounds_and_reference();
    nommu_flat_allocation_ignores_the_request();
    nommu_allocation_failure_and_rollback();
    nommu_vmo_is_unsupported();
    reset_state();

    if (failures) {
        printf("MMAP_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MMAP_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}
