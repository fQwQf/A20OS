/*
 * Host differential harness for kernel/mm/mempolicy_state.rs.
 *
 * The production Rust module is linked against stubs that model the per-task
 * policy storage and the user-window facts.  Every assertion is about a
 * validation decision the Rust module owns: which modes are legal, when a mask
 * is required, which mask is stored, and how far mbind's range contract
 * reaches when the task has no address space.
 *
 * This is a decision oracle, not a kernel; the guest syscall round trip is the
 * separate gate.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define USER_VA_LIMIT 0x4000000000ULL
#define ESRCH 3
#define EINVAL 22

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

static int g_has_mm;
static int g_store_calls;
static int g_stored_mode = -1;
static uint64_t g_stored_nmask;
static int g_store_rc;

int a20_mempolicy_has_mm(const void *t) { (void)t; return g_has_mm; }
uintptr_t a20_mempolicy_user_va_limit(void) { return USER_VA_LIMIT; }
int a20_mempolicy_store(void *t, int mode, uint64_t nmask)
{
    (void)t;
    g_store_calls++;
    g_stored_mode = mode;
    g_stored_nmask = nmask;
    return g_store_rc;
}

extern int mempolicy_set(void *t, int mode, uint64_t nmask);
extern int mempolicy_mbind(void *t, uint64_t addr, unsigned long len, int mode,
                           uint64_t nmask, unsigned flags);

static void reset(void)
{
    g_has_mm = 1;
    g_store_calls = 0;
    g_stored_mode = -1;
    g_stored_nmask = 0;
    g_store_rc = 0;
}

static void set_contract(void)
{
    static int task;

    reset();
    check(mempolicy_set(NULL, 1, 1) == -ESRCH, "set: a null task is ESRCH");
    check(g_store_calls == 0, "set: a null task stores nothing");

    reset();
    check(mempolicy_set(&task, -1, 1) == -EINVAL, "set: a negative mode is EINVAL");
    check(mempolicy_set(&task, 5, 1) == -EINVAL, "set: an out-of-range mode is EINVAL");
    check(g_store_calls == 0, "set: an invalid mode stores nothing");

    reset();
    check(mempolicy_set(&task, 2 /* BIND */, 0) == -EINVAL,
          "set: BIND with an empty mask is EINVAL");
    check(mempolicy_set(&task, 3 /* INTERLEAVE */, 0) == -EINVAL,
          "set: INTERLEAVE with an empty mask is EINVAL");
    check(mempolicy_set(&task, 1 /* PREFERRED */, 0) == 0,
          "set: PREFERRED accepts an empty mask");
    check(g_stored_nmask == 0, "set: PREFERRED stores a zero mask");

    reset();
    check(mempolicy_set(&task, 2, 2) == -EINVAL,
          "set: a mask naming node 1 is EINVAL");
    check(g_store_calls == 0, "set: a mask naming node 1 stores nothing");

    reset();
    check(mempolicy_set(&task, 2, 1) == 0, "set: BIND with node 0 succeeds");
    check(g_stored_mode == 2 && g_stored_nmask == 1,
          "set: BIND stores its mode and mask");

    reset();
    check(mempolicy_set(&task, 0 /* DEFAULT */, 1) == 0,
          "set: DEFAULT succeeds with a mask");
    check(g_stored_mode == 0 && g_stored_nmask == 0,
          "set: DEFAULT drops the mask so a later get cannot report it");

    reset();
    check(mempolicy_set(&task, 4 /* LOCAL */, 0) == 0, "set: LOCAL succeeds");
    check(g_stored_mode == 4 && g_stored_nmask == 0, "set: LOCAL stores no mask");
}

static void mbind_contract(void)
{
    static int task;

    reset();
    check(mempolicy_mbind(NULL, 0x1000, 0x1000, 1, 0, 0) == -ESRCH,
          "mbind: a null task is ESRCH");
    check(mempolicy_mbind(&task, 0, 0x1000, 1, 0, 0) == -EINVAL,
          "mbind: a zero address is EINVAL");
    check(mempolicy_mbind(&task, 0x1000, 0, 1, 0, 0) == -EINVAL,
          "mbind: a zero length is EINVAL");
    check(mempolicy_mbind(&task, 0x1001, 0x1000, 1, 0, 0) == -EINVAL,
          "mbind: an unaligned address is EINVAL");
    check(mempolicy_mbind(&task, 0x1000, 0x1001, 1, 0, 0) == -EINVAL,
          "mbind: an unaligned length is EINVAL");
    check(mempolicy_mbind(&task, 0x1000, 0x1000, 2, 0, 0) == -EINVAL,
          "mbind: BIND with an empty mask is EINVAL");
    check(mempolicy_mbind(&task, 0x1000, 0x1000, 2, 2, 0) == -EINVAL,
          "mbind: a mask naming node 1 is EINVAL");
    check(g_store_calls == 0, "mbind: no refusal stores a policy");

    reset();
    check(mempolicy_mbind(&task, 0x1000, 0x2000, 2, 1, 0) == 0,
          "mbind: a valid range succeeds");
    check(g_stored_mode == 2 && g_stored_nmask == 1,
          "mbind: the policy is stored");

    reset();
    check(mempolicy_mbind(&task, USER_VA_LIMIT - 0x1000, 0x2000, 1, 0, 0) == -EINVAL,
          "mbind: a range past the user limit is EINVAL");
    check(g_store_calls == 0, "mbind: an out-of-window range stores nothing");

    reset();
    check(mempolicy_mbind(&task, 0x1000, ~0UL & ~0xFFFUL, 1, 0, 0) == -EINVAL,
          "mbind: a wrapping range is EINVAL");

    /* With no address space the C reference skipped the range check and still
     * stored the policy; that asymmetry is preserved. */
    reset();
    g_has_mm = 0;
    check(mempolicy_mbind(&task, USER_VA_LIMIT + 0x1000, 0x1000, 1, 0, 0) == 0,
          "mbind: no address space means no range check");
    check(g_store_calls == 1, "mbind: the policy is still stored");
}

int main(void)
{
    set_contract();
    mbind_contract();
    reset();

    if (failures) {
        printf("MEMPOLICY_STATE_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("MEMPOLICY_STATE_HOST: PASS %d checks\n", checks);
    return 0;
}
