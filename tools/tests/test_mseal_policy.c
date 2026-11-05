/*
 * Host differential harness for kernel/mm/mseal_policy.rs.
 *
 * The production Rust sealing transaction is linked against a model of the
 * address space: a sorted record list, a split counter and a mark log.  Every
 * assertion is about a decision the Rust module owns: the coverage walk, the
 * special-region refusal, the split ordering before any mark, and the extents
 * handed to the seal.
 *
 * Guest coverage is separate: mm_stress's mseal phases run the real syscall.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define USER_VA_LIMIT 0x4000000000UL

#define VM_VMO (1UL << 14)
#define VM_SHARED (1UL << 3)
#define VM_ANON (1UL << 4)
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

struct rec {
    uintptr_t start, end;
    uint64_t vm_flags;
};

struct stub_mm {
    struct rec recs[8];
    int n;
    int has_pgdir;
};

struct a20_mseal_rec {
    void *vma;
    uintptr_t start, end;
    uint64_t vm_flags;
};

static struct stub_mm g_mm;
static int split_calls;
static uintptr_t split_addrs[8];
static int mark_calls;
static uintptr_t mark_a[8], mark_b[8];

static void reset(void)
{
    memset(&g_mm, 0, sizeof(g_mm));
    g_mm.has_pgdir = 1;
    split_calls = 0;
    mark_calls = 0;
}

static void add(uintptr_t start, uintptr_t end, uint64_t flags)
{
    int i = g_mm.n++;
    g_mm.recs[i].start = start;
    g_mm.recs[i].end = end;
    g_mm.recs[i].vm_flags = flags;
}

int a20_mseal_has_pgdir(const void *mm) { return ((const struct stub_mm *)mm)->has_pgdir; }
uintptr_t a20_mseal_user_va_limit(void) { return USER_VA_LIMIT; }

int a20_mseal_record_from(const void *mm, uintptr_t va, struct a20_mseal_rec *out)
{
    const struct stub_mm *m = mm;
    for (int i = 0; i < m->n; i++) {
        if (m->recs[i].end <= va)
            continue;
        out->vma = (void *)(uintptr_t)(i + 1);
        out->start = m->recs[i].start;
        out->end = m->recs[i].end;
        out->vm_flags = m->recs[i].vm_flags;
        return 1;
    }
    return 0;
}

int a20_mseal_split_at(void *mm, uintptr_t addr)
{
    (void)mm;
    if (split_calls < 8)
        split_addrs[split_calls] = addr;
    split_calls++;
    return 0;
}

void a20_mseal_mark(void *mm, void *vma, uintptr_t a, uintptr_t b)
{
    struct stub_mm *m = mm;
    int idx = (int)(uintptr_t)vma - 1;
    if (mark_calls < 8) {
        mark_a[mark_calls] = a;
        mark_b[mark_calls] = b;
    }
    mark_calls++;
    if (idx >= 0 && idx < m->n)
        m->recs[idx].vm_flags |= VM_SEALED;
}

extern int mm_mseal_locked(void *mm, uintptr_t addr, uintptr_t len);
extern int mm_mseal_range_is_sealed_locked(void *mm, uintptr_t addr, uintptr_t len);

static void refusals(void)
{
    reset();
    g_mm.has_pgdir = 0;
    check(mm_mseal_locked(&g_mm, 0x1000, PAGE_SIZE) == -22, "no pgdir is EINVAL");

    reset();
    add(0x1000, 0x9000, 0);
    check(mm_mseal_locked(&g_mm, 0x1001, PAGE_SIZE) == -22, "misaligned address is EINVAL");
    check(mm_mseal_locked(&g_mm, 0x1000, 0) == 0, "zero length is a no-op");
    check(split_calls == 0 && mark_calls == 0, "the no-op splits and marks nothing");
    check(mm_mseal_locked(&g_mm, USER_VA_LIMIT - PAGE_SIZE, 2 * PAGE_SIZE) == -12,
          "a range past the user limit is ENOMEM");

    /* A hole at the start and a hole in the middle both refuse. */
    reset();
    add(0x3000, 0x5000, 0);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x4000) == -12, "a leading hole is ENOMEM");
    reset();
    add(0x1000, 0x2000, 0);
    add(0x3000, 0x4000, 0);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x3000) == -12, "an interior hole is ENOMEM");
    check(split_calls == 0 && mark_calls == 0,
          "a coverage refusal splits and marks nothing");

    reset();
    add(0x1000, 0x5000, VM_SYSV_SHM);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x4000) == -1, "a SysV range is EPERM");
    reset();
    add(0x1000, 0x5000, VM_PFNMAP);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x4000) == -1, "a PFNMAP range is EPERM");
    reset();
    add(0x1000, 0x5000, VM_VMO);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x4000) == -1, "a VMO range is EPERM");
    check(split_calls == 0 && mark_calls == 0,
          "a special-region refusal splits and marks nothing");
    reset();
    add(0x1000, 0x5000, VM_VMO | VM_SHARED | VM_ANON);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x4000) == 0,
          "shared anonymous backing remains sealable");
}

static void sealing(void)
{
    reset();
    add(0x1000, 0x9000, 0);
    check(mm_mseal_locked(&g_mm, 0x3000, 0x2000) == 0, "a covered range seals");
    check(split_calls == 2, "both edges are split");
    check(split_addrs[0] == 0x3000 && split_addrs[1] == 0x5000,
          "the splits are at the range edges, start first");
    check(mark_calls == 1, "one record is marked");
    check(mark_a[0] == 0x3000 && mark_b[0] == 0x5000,
          "the mark covers exactly the requested range");
    check((g_mm.recs[0].vm_flags & VM_SEALED) != 0, "the record carries the seal");

    /* Two adjacent records: each is marked with its own clipped extent. */
    reset();
    add(0x1000, 0x3000, 0);
    add(0x3000, 0x5000, 0);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x3000) == 0, "a two-record range seals");
    check(mark_calls == 2, "both records are marked");
    check(mark_a[0] == 0x1000 && mark_b[0] == 0x3000, "the first extent is its own");
    check(mark_a[1] == 0x3000 && mark_b[1] == 0x4000, "the second extent is clipped");

    /* A range that ends exactly on a record boundary. */
    reset();
    add(0x1000, 0x9000, 0);
    check(mm_mseal_locked(&g_mm, 0x1000, 0x8000) == 0, "an exact-record range seals");
    check(split_calls == 2 && mark_calls == 1, "the boundary case still splits once");
}

static void query(void)
{
    reset();
    add(0x1000, 0x3000, VM_SEALED);
    add(0x3000, 0x5000, 0);
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x1000, 0x1000) == 1,
          "a sealed record is reported");
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x2000, 0x2000) == 1,
          "an overlapping sealed record is reported");
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x3000, 0x2000) == 0,
          "an unsealed neighbour is not reported");
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x5000, 0x1000) == 0,
          "a range past the records is not reported");
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x800, 0x800) == 0,
          "a range ending at the record start is not reported");
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x1000, 0x8000) == 1,
          "a wide range that touches the sealed record is reported");

    /* A hole before the sealed record must not hide it: the query walks past
     * holes, which the coverage check treats as a refusal. */
    reset();
    add(0x3000, 0x5000, VM_SEALED);
    check(mm_mseal_range_is_sealed_locked(&g_mm, 0x1000, 0x4000) == 1,
          "a hole does not hide a sealed record behind it");

    check(mm_mseal_range_is_sealed_locked(NULL, 0x1000, 0x1000) == 0,
          "a null mm is not sealed");
}

int main(void)
{
    refusals();
    sealing();
    query();
    reset();

    if (failures) {
        printf("MSEAL_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MSEAL_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}
