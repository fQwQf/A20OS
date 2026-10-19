/*
 * Host differential harness for the mount-namespace lifecycle engine
 * (VFS_MNTNS_IMPL).
 *
 * Drives a C reference transcription of the pre-migration mntns.c admission and
 * shutdown logic and the Rust engine (kernel/fs/vfs/mntns_lifecycle.rs, linked
 * as a host staticlib) over the same deterministic op stream, comparing the
 * return value and every observable counter after each op.
 *
 * Exit 0 on success; on mismatch prints the op and both states, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EBUSY 16
#define ESHUTDOWN 108
#define EINVAL 22
#define MNTNS_SHUTDOWN_MAX 256u

typedef struct {
    uint32_t create_inflight;
    uint32_t mutations_inflight;
    uint32_t fileops_inflight;
    uint32_t shutdown_active;
    uint32_t shutdown_count;
    uint32_t shutdown_cap;
    uint64_t teardown_pass;
} mntns_lifecycle_t;

typedef int (*mntns_scan_fn)(void *ctx, uint32_t cap, uint32_t *out_count);

typedef struct {
    uint32_t teardown_pending;
    uint32_t teardown_running;
    uint32_t retryable;
    uint32_t _pad;
    uint64_t attempt_pass;
} mntns_retry_cand_t;

extern void a20_mntns_lifecycle_init(mntns_lifecycle_t *st, uint32_t cap);
extern int a20_mntns_create_enter(mntns_lifecycle_t *st);
extern uint32_t a20_mntns_create_leave(mntns_lifecycle_t *st);
extern int a20_mntns_mutation_enter(mntns_lifecycle_t *st);
extern uint32_t a20_mntns_mutation_leave(mntns_lifecycle_t *st);
extern int a20_mntns_fileop_enter(mntns_lifecycle_t *st);
extern uint32_t a20_mntns_fileop_leave(mntns_lifecycle_t *st);
extern int a20_mntns_shutdown_active(const mntns_lifecycle_t *st);
extern int a20_mntns_shutdown_count(const mntns_lifecycle_t *st);
extern int a20_mntns_shutdown_begin(mntns_lifecycle_t *st, void *ctx, mntns_scan_fn scan);
extern int a20_mntns_shutdown_abort_plan(mntns_lifecycle_t *st);
extern uint64_t a20_mntns_retry_pass(mntns_lifecycle_t *st);
extern int a20_mntns_retry_select(const mntns_retry_cand_t *cands, size_t n,
                                  uint64_t pass, int allow_busy_retry);

/* ---- C reference model (pre-migration mntns.c logic) ---- */

typedef struct {
    int shutdown_active;
    int shutdown_count;
    unsigned int create_inflight;
    unsigned int mutations_inflight;
    unsigned int fileops_inflight;
    uint64_t teardown_pass;
    /* what the scan callback reports */
    int scan_rc;
    uint32_t scan_count;
    int scan_calls;
    unsigned int scan_pins;
} RefState;

static int ref_create_enter(RefState *r)
{
    if (r->shutdown_active) {
        return -EBUSY;
    }
    r->create_inflight++;
    return 0;
}
static void ref_create_leave(RefState *r)
{
    if (r->create_inflight)
        r->create_inflight--;
}
static int ref_mutation_enter(RefState *r)
{
    if (r->shutdown_active)
        return -ESHUTDOWN;
    r->mutations_inflight++;
    return 0;
}
static void ref_mutation_leave(RefState *r)
{
    if (r->mutations_inflight)
        r->mutations_inflight--;
}
static int ref_fileop_enter(RefState *r)
{
    if (r->shutdown_active)
        return -ESHUTDOWN;
    r->fileops_inflight++;
    return 0;
}
static void ref_fileop_leave(RefState *r)
{
    if (r->fileops_inflight)
        r->fileops_inflight--;
}
static int ref_shutdown_active(const RefState *r) { return r->shutdown_active; }
static int ref_shutdown_count(const RefState *r)
{
    return r->shutdown_active ? r->shutdown_count : 0;
}
static int ref_shutdown_begin(RefState *r)
{
    if (r->shutdown_active || r->create_inflight || r->mutations_inflight ||
        r->fileops_inflight) {
        return -EBUSY;
    }
    /* the adapter's scan: capacity/teardown check, then pin */
    r->scan_calls++;
    if (r->scan_rc != 0)
        return r->scan_rc;
    if (r->scan_count > MNTNS_SHUTDOWN_MAX)
        return -EBUSY;
    r->scan_pins += r->scan_count;
    r->shutdown_count = (int)r->scan_count;
    r->shutdown_active = 1;
    return 0;
}
static int ref_shutdown_abort(RefState *r)
{
    if (!r->shutdown_active)
        return -EINVAL;
    int n = r->shutdown_count;
    r->shutdown_count = 0;
    r->shutdown_active = 0;
    return n;
}
static uint64_t ref_retry_pass(RefState *r)
{
    uint64_t pass = ++r->teardown_pass;
    if (pass == 0)
        pass = ++r->teardown_pass;
    return pass;
}
static int ref_retry_select(const mntns_retry_cand_t *c, size_t n, uint64_t pass,
                            int allow_busy)
{
    for (size_t i = 0; i < n; i++) {
        if (c[i].teardown_pending && !c[i].teardown_running &&
            c[i].attempt_pass != pass && (allow_busy || c[i].retryable)) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- shared scan-callback stub for the Rust side ---- */
static int g_scan_rc;
static uint32_t g_scan_count;
static int g_scan_calls;
static unsigned int g_scan_pins;

static int scan_stub(void *ctx, uint32_t cap, uint32_t *out)
{
    (void)ctx;
    g_scan_calls++;
    if (g_scan_rc != 0)
        return g_scan_rc;
    if (g_scan_count > cap)
        return -EBUSY;
    g_scan_pins += g_scan_count;
    *out = g_scan_count;
    return 0;
}

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

static int cmp_state(const RefState *r, const mntns_lifecycle_t *st, unsigned step, unsigned op)
{
    int count = r->shutdown_active ? r->shutdown_count : 0;
    if ((int)st->shutdown_active != r->shutdown_active ||
        (int)st->create_inflight != (int)r->create_inflight ||
        (int)st->mutations_inflight != (int)r->mutations_inflight ||
        (int)st->fileops_inflight != (int)r->fileops_inflight ||
        a20_mntns_shutdown_count(st) != count ||
        st->teardown_pass != r->teardown_pass) {
        printf("mntns state mismatch step %u op %u\n"
               "  C:    active=%d count=%d create=%u mut=%u fileop=%u pass=%llu\n"
               "  RUST: active=%u count=%d create=%u mut=%u fileop=%u pass=%llu\n",
               step, op, r->shutdown_active, count, r->create_inflight,
               r->mutations_inflight, r->fileops_inflight,
               (unsigned long long)r->teardown_pass, st->shutdown_active,
               a20_mntns_shutdown_count(st), st->create_inflight,
               st->mutations_inflight, st->fileops_inflight,
               (unsigned long long)st->teardown_pass);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0x082efa98ec4e6c89ull;

    RefState rs;
    memset(&rs, 0, sizeof(rs));
    mntns_lifecycle_t st;
    a20_mntns_lifecycle_init(&st, MNTNS_SHUTDOWN_MAX);

    mntns_retry_cand_t cand[8];
    static const uint32_t caps[] = { 0, 1, 2, 8, 255, 256, 257, 4096, 0xffffffffu };

    for (unsigned step = 0; step < rounds; step++) {
        unsigned op = (unsigned)(rnd() % 12);
        int ra = 0, rb = 0;
        int cmp = 0;

        /* the scan stub's answer is shared, so both sides see the same one */
        g_scan_count = rs.scan_count;
        g_scan_rc = rs.scan_rc;
        g_scan_calls = rs.scan_calls;
        g_scan_pins = rs.scan_pins;

        switch (op) {
        case 0: ra = ref_create_enter(&rs); rb = a20_mntns_create_enter(&st); break;
        case 1: ref_create_leave(&rs); a20_mntns_create_leave(&st); break;
        case 2: ra = ref_mutation_enter(&rs); rb = a20_mntns_mutation_enter(&st); break;
        case 3: ref_mutation_leave(&rs); a20_mntns_mutation_leave(&st); break;
        case 4: ra = ref_fileop_enter(&rs); rb = a20_mntns_fileop_enter(&st); break;
        case 5: ref_fileop_leave(&rs); a20_mntns_fileop_leave(&st); break;
        case 6: ra = ref_shutdown_active(&rs); rb = a20_mntns_shutdown_active(&st); break;
        case 7: ra = ref_shutdown_begin(&rs); rb = a20_mntns_shutdown_begin(&st, NULL, scan_stub);
                if (g_scan_pins != rs.scan_pins) {
                    printf("mntns scan-pin mismatch step %u: %u vs %u\n", step,
                           rs.scan_pins, g_scan_pins);
                    return 1;
                }
                break;
        case 8: ra = ref_shutdown_abort(&rs); rb = a20_mntns_shutdown_abort_plan(&st); break;
        case 9: ra = ref_shutdown_count(&rs); rb = a20_mntns_shutdown_count(&st); break;
        case 10: {
            uint64_t a = ref_retry_pass(&rs);
            uint64_t b = a20_mntns_retry_pass(&st);
            if (a != b) {
                printf("mntns pass mismatch step %u: %llu vs %llu\n", step,
                       (unsigned long long)a, (unsigned long long)b);
                return 1;
            }
            break;
        }
        default: {
            int n = (int)(rnd() % 8);
            for (int i = 0; i < n; i++) {
                cand[i].teardown_pending = (uint32_t)(rnd() % 2);
                cand[i].teardown_running = (uint32_t)(rnd() % 2);
                cand[i].retryable = (uint32_t)(rnd() % 2);
                cand[i]._pad = 0;
                cand[i].attempt_pass = (rnd() % 3 == 0) ? rs.teardown_pass : rnd() % 3;
            }
            uint64_t pass = rs.teardown_pass;
            int allow = (int)(rnd() % 2);
            ra = ref_retry_select(cand, (size_t)n, pass, allow);
            rb = a20_mntns_retry_select(cand, (size_t)n, pass, allow);
            break;
        }
        }

        if (ra != rb) {
            printf("mntns rc mismatch step %u op %u: C=%d RUST=%d\n", step, op, ra, rb);
            return 1;
        }
        if (cmp_state(&rs, &st, step, op))
            return 1;

        if (rnd() % 32 == 0) {
            rs.scan_rc = (rnd() % 4 == 0) ? -EBUSY : 0;
            rs.scan_count = caps[rnd() % (sizeof(caps) / sizeof(caps[0]))];
        }
        (void)cmp;
    }

    printf("vfs mntns lifecycle C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
