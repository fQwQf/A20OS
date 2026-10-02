#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Child exit-code contract:
 *   0   passed
 *   77  SKIP    -- configured out in this run (environment).  Counted, printed
 *                  by name, and the suite's exit code becomes 77 so no caller
 *                  mistakes it for a clean pass.
 *   78  ABSENT  -- the capability is not implemented.  Never tolerated UNLESS
 *                  the test is declared known_absent below: the suite fails,
 *                  because a commented-out kernel implementation that leaves
 *                  the gate green is the failure mode this file exists to
 *                  remove.
 *   *   failed.
 *
 * known_absent is a different claim from may_skip, and the difference matters.
 * may_skip says "this run's environment did not exercise the capability", which
 * is a fact about the run.  known_absent says "the project has deliberately
 * decided this capability does not exist", which is a fact about the tree that
 * the gate is supposed to pin.  A test may only carry known_absent if its
 * kernel side documents the non-goal, and then its ABSENT result is the
 * asserted state: reported by name, counted separately, and never a failure.
 * Without that distinction a documented non-goal makes the gate permanently
 * red, and a permanently red gate is a gate nobody runs -- which is strictly
 * worse for coverage than an honest "10 passed, 1 declared-absent".
 *
 * The fail-closed direction is preserved in both directions.  A test that is
 * NOT declared known_absent and goes absent still fails the suite.  A test that
 * IS declared known_absent and starts passing does not fail either -- that is
 * an improvement, not a regression -- but it is reported as
 * PRESENT(expected-absent) so the stale declaration gets removed instead of
 * rotting.
 */
#define SKIP_CODE 77
#define ABSENT_CODE 78
/* Internal verdicts, numbered clear of every code a child can actually return
 * (0, 1, 77, 78, 127) so they cannot be mistaken for a real exit status. */
#define EXPECTED_ABSENT_CODE 79
#define NOW_PRESENT_CODE 80

typedef struct {
    const char *name;
    const char *cmd;
    int may_skip;
    int known_absent;
} test_case_t;

static const test_case_t tests[] = {
    { "dns_test",          "dns_test localhost",       1, 0 },
    { "tcp_loopback_test", "tcp_loopback_test",        0, 0 },
    { "tcp_edge_test",     "tcp_edge_test",            0, 0 },
    { "udp_loopback_test", "udp_loopback_test",        0, 0 },
    { "icmp_loopback_test","icmp_loopback_test",       0, 0 },
    { "unix_test",         "unix_test",                0, 0 },
    /* AF_ALG ships an empty algorithm registry on purpose (see the AF_ALG note
     * in kernel/net/socket_alg.c), so alg_test honestly reports ABSENT and the
     * gate has to assert that rather than be defeated by it. */
    { "alg_test",          "alg_test",                 0, 1 },
    { "timeout_test",      "timeout_test",             0, 0 },
    { "net_iface_test",    "net_iface_test",           0, 0 },
    { "netopt_test",       "netopt_test",              0, 0 },
    { "netlink_test",      "netlink_test",             0, 0 },
};

static int run_test(const test_case_t *t) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", t->cmd, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return t->known_absent ? NOW_PRESENT_CODE : 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == ABSENT_CODE)
        return t->known_absent ? EXPECTED_ABSENT_CODE : ABSENT_CODE;
    if (t->may_skip && WIFEXITED(status) && WEXITSTATUS(status) == SKIP_CODE)
        return SKIP_CODE;
    return 1;
}

static void print_named(const char *label, const int *hits, int n) {
    for (int i = 0; i < n; i++)
        printf("NETWORK_SUITE: %s %s\n", label, tests[hits[i]].name);
}

int main(void) {
    enum { N = (int)(sizeof(tests) / sizeof(tests[0])) };
    int passed = 0;
    int failed = 0;
    int skipped_hits[N];
    int absent_hits[N];
    int expected_absent_hits[N];
    int now_present_hits[N];
    int skipped = 0;
    int absent = 0;
    int expected_absent = 0;
    int now_present = 0;
    int failed_hits[N];

    for (int i = 0; i < N; i++) {
        int r = run_test(&tests[i]);
        if (r == 0) {
            passed++;
        } else if (r == SKIP_CODE) {
            skipped_hits[skipped++] = i;
        } else if (r == EXPECTED_ABSENT_CODE) {
            expected_absent_hits[expected_absent++] = i;
        } else if (r == NOW_PRESENT_CODE) {
            now_present_hits[now_present++] = i;
        } else if (r == ABSENT_CODE) {
            absent_hits[absent++] = i;
        } else {
            failed_hits[failed++] = i;
        }
    }

    /* The PASS line is no longer a lie about coverage.
     *
     * failed  -> FAIL, exit 1.
     * absent  -> FAIL, exit 1.  A capability that is not implemented must never
     *            ride along inside a green gate; this is the state that makes
     *            commenting out a kernel implementation redden the gate.
     * skipped -> PASS *with the count and the names printed*, exit 77.  These
     *            are environment facts (no resolver reply in this run), not
     *            missing kernel code, so red here would be a false alarm; but
     *            the exit code says "incomplete" to any caller that consults it,
     *            so the fact is never swallowed.
     * declared-absent -> PASS, and named on the PASS line.  A documented
     *            non-goal is a fact about the tree, not a coverage hole, so it
     *            is reported and excluded from the exit code rather than
     *            reddening the gate forever.  See the known_absent note above
     *            for why this cannot be folded into skipped.
     */
    if (failed)
        print_named("FAIL", failed_hits, failed);
    if (absent)
        print_named("ABSENT", absent_hits, absent);
    if (skipped)
        print_named("SKIP", skipped_hits, skipped);
    if (expected_absent)
        print_named("DECLARED-ABSENT", expected_absent_hits, expected_absent);

    if (now_present) {
        print_named("PRESENT(expected-absent)", now_present_hits, now_present);
        printf("NETWORK_SUITE: note -- a test declared known_absent now passes, "
               "so its kernel non-goal is stale; drop the declaration\n");
    }

    if (failed || absent) {
        printf("NETWORK_SUITE: FAIL (%d passed, %d skipped, %d absent, "
               "%d declared-absent, %d failed)\n",
               passed, skipped, absent, expected_absent, failed);
        return 1;
    }
    if (skipped) {
        printf("NETWORK_SUITE: PASS (%d passed, %d skipped, %d declared-absent; "
               "coverage incomplete)\n", passed, skipped, expected_absent);
        return SKIP_CODE;
    }
    printf("NETWORK_SUITE: PASS (%d passed, %d declared-absent)\n",
           passed, expected_absent);
    return 0;
}
