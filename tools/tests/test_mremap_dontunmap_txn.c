#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "mm/mremap_dontunmap_txn.h"

enum { EVENT_LIMIT = 16, FAILURE_COUNT = 8 };
_Static_assert(sizeof(mm_dontunmap_txn_ops_t) == 9 * sizeof(void *),
               "Rust Option<fn> and C nullable function pointer layout must match");
_Static_assert(offsetof(mm_dontunmap_txn_ops_t, rollback_failed) ==
                   8 * sizeof(void *),
               "Rust/C ops callback field order must match");
typedef int (*runner_t)(const mm_dontunmap_txn_ops_t *, void *, uintptr_t,
                        size_t, uintptr_t *);

typedef struct fake_mm {
    int fail_stage;
    int cleanup_error;
    int events[EVENT_LIMIT];
    int event_count;
    int rollback_calls;
    int rollback_exact;
    int rollback_failures;
    int logged_primary;
    int logged_cleanup;
    int source_cleared;
    int source_lock_released;
} fake_mm_t;

static void event(fake_mm_t *f, int e)
{
    if (f->event_count < EVENT_LIMIT)
        f->events[f->event_count++] = e;
}

static int step(fake_mm_t *f, int stage, int e)
{
    event(f, e);
    if (f->fail_stage != stage) return 0;
    return stage <= 4 ? -12 : -100 - stage; /* split allocation: -ENOMEM */
}

static int prepare(void *ctx)
{
    fake_mm_t *f = ctx;
    /* Production preparation makes four split/revalidation calls: destination
     * start/end, then source start/end. Inject each independently to retain
     * every partial-preparation rollback state in this adapter test. */
    int r = step(f, 1, 1); /* split at destination start */
    if (r < 0) return r;
    r = step(f, 2, 2);     /* split at destination end */
    if (r < 0) return r;
    r = step(f, 3, 3);     /* split at source start */
    if (r < 0) return r;
    r = step(f, 4, 4);     /* split at source end */
    if (r < 0) return r;
    return 0;
}

static int copy_backing(void *ctx) { return step(ctx, 5, 5); }
static int move_pages(void *ctx) { return step(ctx, 6, 6); }
static int reprotect(void *ctx) { return step(ctx, 7, 7); }
static int preflight(void *ctx) { return step(ctx, 8, 8); }

static void clear_source(void *ctx)
{
    fake_mm_t *f = ctx;
    event(f, 9);
    f->source_cleared = 1;
}

static void release_source_lock(void *ctx)
{
    fake_mm_t *f = ctx;
    event(f, 10);
    f->source_lock_released = 1;
}

static int rollback(void *ctx, int exact)
{
    fake_mm_t *f = ctx;
    event(f, exact ? 102 : 101);
    f->rollback_calls++;
    f->rollback_exact = exact;
    return f->cleanup_error;
}

static void rollback_failed(void *ctx, int primary, int cleanup)
{
    fake_mm_t *f = ctx;
    f->rollback_failures++;
    f->logged_primary = primary;
    f->logged_cleanup = cleanup;
}

static const mm_dontunmap_txn_ops_t ops = {
    .prepare_destination = prepare,
    .copy_backing = copy_backing,
    .move_pages = move_pages,
    .reprotect = reprotect,
    .preflight_source = preflight,
    .clear_source = clear_source,
    .release_source_lock = release_source_lock,
    .rollback_destination = rollback,
    .rollback_failed = rollback_failed,
};

static void reset(fake_mm_t *f, int fail, int cleanup)
{
    memset(f, 0, sizeof(*f));
    f->fail_stage = fail;
    f->cleanup_error = cleanup;
}

static int check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

static int run_suite(runner_t run, const char *label)
{
    int bad = 0;
    const uintptr_t dst = 0x400000;
    const uintptr_t sentinel = 0xdeadbeef;
    for (int fail = 1; fail <= FAILURE_COUNT; fail++) {
        fake_mm_t f;
        reset(&f, fail, 0);
        uintptr_t out = sentinel;
        int r = run(&ops, &f, dst, 8192, &out);
        int exact = fail >= 5;
        int expected_error = fail <= 4 ? -12 : -100 - fail;
        bad += check(r == expected_error, "primary error preserved after successful rollback");
        bad += check(out == sentinel, "failure does not publish destination");
        bad += check(f.rollback_calls == 1 && f.rollback_exact == exact,
                     "one rollback with correct pre/post split mode");
        bad += check(!f.source_cleared && !f.source_lock_released,
                     "failure before commit preserves source");
        bad += check(f.rollback_failures == 0, "successful cleanup has no rollback diagnostic");
    }

    for (int fail = 1; fail <= FAILURE_COUNT; fail++) {
        fake_mm_t f;
        reset(&f, fail, -77);
        uintptr_t out = sentinel;
        int r = run(&ops, &f, dst, 8192, &out);
        bad += check(r == -77, "cleanup error is the returned error");
        bad += check(f.rollback_failures == 1 &&
                     f.logged_primary == (fail <= 4 ? -12 : -100 - fail) &&
                     f.logged_cleanup == -77,
                     "primary and cleanup errors both remain observable");
        bad += check(!f.source_cleared && !f.source_lock_released,
                     "failed rollback does not commit source");
    }

    fake_mm_t f;
    reset(&f, 0, 0);
    uintptr_t out = sentinel;
    int r = run(&ops, &f, dst, 8192, &out);
    static const int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    bad += check(r == 0 && out == dst, "success publishes destination");
    bad += check(f.rollback_calls == 0 && f.rollback_failures == 0,
                 "successful commit disarms rollback");
    bad += check(f.source_cleared && f.source_lock_released,
                 "source clear and accounting follow preflight");
    bad += check(f.event_count == (int)(sizeof(expected) / sizeof(expected[0])) &&
                 memcmp(f.events, expected, sizeof(expected)) == 0,
                 "success phase order is exact");

    if (bad)
        fprintf(stderr, "%s: %d failures\n", label, bad);
    else
        printf("%s: PASS stages=%d failures=%d cleanup=observable\n",
               label, FAILURE_COUNT, FAILURE_COUNT);
    return bad;
}

static int test_invalid_ops(runner_t run, const char *label)
{
    fake_mm_t f;
    reset(&f, 0, 0);
    uintptr_t out = 0xdeadbeef;
    mm_dontunmap_txn_ops_t invalid = ops;
    invalid.move_pages = NULL;
    int r = run(&invalid, &f, 0x400000, 8192, &out);
    int bad = check(r == -22, "null callback rejected");
    bad += check(f.event_count == 0 && f.rollback_calls == 0,
                 "invalid callback rejected before transaction ownership");
    if (bad) fprintf(stderr, "%s invalid ops: %d failures\n", label, bad);
    return bad;
}

static int test_zero_destination(runner_t run, const char *label)
{
    fake_mm_t f;
    reset(&f, 0, 0);
    uintptr_t out = 0xdeadbeef;
    int r = run(&ops, &f, 0, 8192, &out);
    int bad = check(r == 0 && out == 0,
                    "address zero is accepted as an allocated destination");
    bad += check(f.rollback_calls == 0 && f.source_cleared &&
                 f.source_lock_released,
                 "zero destination follows normal commit sequence");
    if (bad) fprintf(stderr, "%s zero destination: %d failures\n", label, bad);
    return bad;
}

int main(void)
{
    extern int mm_mremap_dontunmap_txn_rust(const mm_dontunmap_txn_ops_t *,
                                            void *, uintptr_t, size_t,
                                            uintptr_t *);
    int bad = run_suite(mm_mremap_dontunmap_txn_c, "DONTUNMAP_C_ORACLE");
    bad += run_suite(mm_mremap_dontunmap_txn_rust, "DONTUNMAP_RUST");
    bad += test_invalid_ops(mm_mremap_dontunmap_txn_c, "DONTUNMAP_C_ORACLE");
    bad += test_invalid_ops(mm_mremap_dontunmap_txn_rust, "DONTUNMAP_RUST");
    bad += test_zero_destination(mm_mremap_dontunmap_txn_c, "DONTUNMAP_C_ORACLE");
    bad += test_zero_destination(mm_mremap_dontunmap_txn_rust, "DONTUNMAP_RUST");
    return bad ? 1 : 0;
}
