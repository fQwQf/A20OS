/*
 * Host differential harness for kernel/mm/cow_status.rs.
 *
 * The production Rust is linked against a scripted model of the status-led COW
 * break: a cursor whose open/replace outcomes the scenario chooses, and
 * counters for every primitive.  The assertions are about the decisions Rust
 * owns -- the decline conditions and which of them still owe the snapshot pin,
 * the non-reclaiming allocation, the rollback that keeps the frame and the
 * charge in step, the "a lost replace still resolves the fault" rule, and the
 * deferred-put contract with the caller.
 *
 * Guest coverage is separate: mm_stress's fork-COW phases must keep
 * cow_from_status non-zero (the smoke case asserts that), so the real path is
 * exercised and cannot go vacuous.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define PFN_NONE 0xFFFFFFFFu
#define MM_ST_COW_BIT 0x08u
#define MM_ST_ANON_MAPPED 2u
#define MM_SAFE_UFFD (1u << 0)

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

struct ctx_words { uint64_t w[16]; };

static int open_rc, open_cls, open_uffd, open_calls, close_calls;
static uintptr_t open_pa;
static int replace_rc, replace_calls, pfn_valid_rc;
static uint32_t alloc_rc, pfn_of_rc, alloc_calls;
static int charge_rc, charge_calls, uncharge_calls;
static int copy_calls, bump_calls, valid_calls;
static int frame_puts, puts_while_open;
static uint32_t last_copy_dst, last_copy_src;
static int g_open_depth;
static uintptr_t last_replace_expect, last_replace_new;
static int last_replace_cls;
static int phys_calls;

static void model_reset(void)
{
    open_rc = 1; open_cls = MM_ST_COW_BIT; open_uffd = 0; open_calls = 0;
    close_calls = 0; open_pa = 0x80000000ULL;
    replace_rc = 0; replace_calls = 0; pfn_valid_rc = 1;
    alloc_rc = 0x1234; pfn_of_rc = 0x1234; alloc_calls = 0;
    charge_rc = 0; charge_calls = 0; uncharge_calls = 0;
    copy_calls = 0; bump_calls = 0; valid_calls = 0;
    frame_puts = 0; puts_while_open = 0;
    last_copy_dst = last_copy_src = 0;
    g_open_depth = 0;
    last_replace_expect = last_replace_new = 0;
    last_replace_cls = -1;
    phys_calls = 0;
}

/* --------------------------------------------------------------- bridges - */
/* a20_fault_status_alloc/phys/charge/uncharge are shared with the demand-fault
 * path (kernel/mm/fault_status.rs) and live in the C bridge block. */

int a20_cowf_status_open(void *ctx, void *mm, uintptr_t va, uint8_t *cls_out,
                         uintptr_t *pa_out, int *uffd_out)
{
    (void)ctx; (void)mm; (void)va;
    open_calls++;
    if (!open_rc) {
        *cls_out = 0; *pa_out = 0; *uffd_out = 0;
        return 0;
    }
    g_open_depth++;
    *cls_out = (uint8_t)open_cls;
    *pa_out = open_pa;
    *uffd_out = open_uffd;
    return 1;
}

int a20_cowf_status_replace(void *ctx, uintptr_t va, uintptr_t expect_pa,
                            uintptr_t new_pa, uint8_t cls, uintptr_t *replaced_out)
{
    (void)ctx; (void)va;
    replace_calls++;
    last_replace_expect = expect_pa;
    last_replace_new = new_pa;
    last_replace_cls = cls;
    if (replaced_out) *replaced_out = 0;
    return replace_rc;
}

void a20_cowf_status_close(void *ctx)
{
    (void)ctx;
    close_calls++;
    g_open_depth--;
}

uint32_t a20_fault_status_alloc(void)
{
    alloc_calls++;
    return alloc_rc;
}

uint32_t a20_cowf_status_pfn_of(uintptr_t pa)
{
    (void)pa;
    return pfn_of_rc;
}

int a20_cowf_status_pfn_valid(uint32_t pfn)
{
    (void)pfn;
    valid_calls++;
    return pfn_valid_rc;
}

uintptr_t a20_fault_status_phys(uint32_t pfn)
{
    phys_calls++;
    return (uintptr_t)pfn << 12;
}

int a20_fault_status_charge(void *t)
{
    (void)t;
    charge_calls++;
    return charge_rc;
}

void a20_fault_status_uncharge(void *t)
{
    (void)t;
    uncharge_calls++;
}

void a20_cowf_status_copy(uint32_t dst, uint32_t src)
{
    copy_calls++;
    last_copy_dst = dst; last_copy_src = src;
}

void a20_cowf_status_bump(void *t)
{
    (void)t;
    bump_calls++;
}

void frame_put(uint32_t pfn)
{
    (void)pfn;
    frame_puts++;
    if (g_open_depth != 0) puts_while_open++;
}

extern int a20_cow_from_status_rust(void *t, void *mm, uintptr_t page_va,
                                    uint32_t *retired_pfn);

/* ------------------------------------------------------------- scenarios - */

static void *const T = (void *)(uintptr_t)0x7A0;
static void *const MM = (void *)(uintptr_t)0x9A0;

static int run(uint32_t *retired)
{
    *retired = 0xDEAD;
    return a20_cow_from_status_rust(T, MM, 0x10000, retired);
}

static void declines(void)
{
    uint32_t retired;

    /* No leaf: nothing was pinned, so nothing is released. */
    model_reset();
    open_rc = 0;
    check(run(&retired) == 0, "decline: an absent leaf declines");
    check(retired == PFN_NONE, "decline: an absent leaf reports no retired frame");
    check(frame_puts == 0 && close_calls == 0, "decline: nothing to release before the pin");
    check(alloc_calls == 0 && replace_calls == 0, "decline: an absent leaf allocates nothing");

    /* Not a COW leaf: the pin is given back. */
    model_reset();
    open_cls = MM_ST_ANON_MAPPED;
    check(run(&retired) == 0, "decline: a non-COW leaf declines");
    check(frame_puts == 1, "decline: the snapshot pin is given back");
    check(close_calls == 1, "decline: the cursor is closed");
    check(alloc_calls == 0 && charge_calls == 0, "decline: a non-COW leaf allocates nothing");

    /* A userfaultfd-marked page is left to the uffd path, pin released. */
    model_reset();
    open_uffd = 1;
    check(run(&retired) == 0, "decline: a uffd-marked page declines");
    check(frame_puts == 1 && close_calls == 1, "decline: the uffd pin is given back");
    check(copy_calls == 0, "decline: a uffd-marked page is not copied");

    /* An invalid pfn after a successful pin: pin released, nothing allocated. */
    model_reset();
    pfn_valid_rc = 0;
    check(run(&retired) == 0, "decline: an invalid pfn declines");
    check(valid_calls == 1, "decline: the pfn is validated once");
    check(frame_puts == 1 && close_calls == 1, "decline: the pin is given back");
    check(alloc_calls == 0, "decline: no frame is allocated for an invalid pfn");

    /* Allocation failure: pin released, no charge. */
    model_reset();
    alloc_rc = PFN_NONE;
    check(run(&retired) == 0, "decline: an allocation failure declines");
    check(charge_calls == 0, "decline: nothing is charged when the allocation failed");
    check(frame_puts == 1, "decline: only the snapshot pin is released");
    check(close_calls == 1, "decline: the cursor is closed after an allocation failure");

    /* Charge failure: both the new frame and the pin are released. */
    model_reset();
    charge_rc = -1;
    check(run(&retired) == 0, "decline: a charge failure declines");
    check(charge_calls == 1 && uncharge_calls == 0, "decline: a failed charge is not undone");
    check(frame_puts == 2, "decline: the new frame and the pin are both released");
    check(copy_calls == 0, "decline: nothing is copied when the charge failed");
}

static void success(void)
{
    uint32_t retired;

    model_reset();
    open_pa = 0x80000000ULL;
    pfn_of_rc = 0x1234;
    alloc_rc = 0x5678;
    check(run(&retired) == 1, "success: a COW leaf is broken");
    check(retired == 0x1234, "success: the retired frame is the pinned one");
    check(close_calls == 1, "success: the cursor is closed before returning");
    check(charge_calls == 1 && uncharge_calls == 0, "success: the charge is kept");
    check(frame_puts == 0, "success: the puts are deferred to the caller");
    check(copy_calls == 1 && last_copy_dst == 0x5678 && last_copy_src == 0x1234,
          "success: the pinned frame is copied into the new one");
    check(replace_calls == 1 && last_replace_expect == 0x80000000ULL &&
          last_replace_new == ((uintptr_t)0x5678 << 12),
          "success: the replace is compare-and-swap against the observed frame");
    check(last_replace_cls == MM_ST_ANON_MAPPED,
          "success: the leaf is published as an anon-mapped page");
    check(bump_calls == 1, "success: the counters are bumped once");
    check(puts_while_open == 0, "success: nothing is released while the cursor is open");
}

static void lost_replace(void)
{
    uint32_t retired;

    /* State moved underneath us: the fault is resolved anyway, and both the
     * copy and the pin are given back. */
    model_reset();
    replace_rc = -1;
    check(run(&retired) == 1, "lost replace: the fault is still resolved");
    check(retired == PFN_NONE, "lost replace: no frame is retired to the caller");
    check(frame_puts == 2, "lost replace: the copy and the pin are both released");
    check(uncharge_calls == 1, "lost replace: the charge is undone");
    check(bump_calls == 0, "lost replace: no counter is bumped for a lost race");
    check(close_calls == 1, "lost replace: the cursor is closed");

    /* Every path closes the cursor exactly once. */
    model_reset();
    check(run(&retired) == 1, "lost replace: closed exactly once on success");
    check(close_calls == 1, "lost replace: one close on the success path");
}

int main(void)
{
    declines();
    success();
    lost_replace();

    if (failures) {
        printf("COW_STATUS_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("COW_STATUS_HOST: PASS %d checks\n", checks);
    return 0;
}
