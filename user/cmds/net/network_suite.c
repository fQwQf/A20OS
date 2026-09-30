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
 *   78  ABSENT  -- the capability is not implemented.  Never tolerated: the
 *                  suite fails, because a commented-out kernel implementation
 *                  that leaves the gate green is the failure mode this file
 *                  exists to remove.
 *   *   failed.
 */
#define SKIP_CODE 77
#define ABSENT_CODE 78

typedef struct {
    const char *name;
    const char *cmd;
    int may_skip;
} test_case_t;

static const test_case_t tests[] = {
    { "dns_test",          "dns_test localhost",       1 },
    { "tcp_loopback_test", "tcp_loopback_test",        0 },
    { "tcp_edge_test",     "tcp_edge_test",            0 },
    { "udp_loopback_test", "udp_loopback_test",        0 },
    { "icmp_loopback_test","icmp_loopback_test",       0 },
    { "unix_test",         "unix_test",                0 },
    { "alg_test",          "alg_test",                 1 },
    { "timeout_test",      "timeout_test",             0 },
    { "net_iface_test",    "net_iface_test",           0 },
    { "netopt_test",       "netopt_test",              0 },
    { "netlink_test",      "netlink_test",             0 },
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
        return 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == ABSENT_CODE)
        return ABSENT_CODE;
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
    int skipped = 0;
    int absent = 0;
    int failed_hits[N];

    for (int i = 0; i < N; i++) {
        int r = run_test(&tests[i]);
        if (r == 0) {
            passed++;
        } else if (r == SKIP_CODE) {
            skipped_hits[skipped++] = i;
        } else if (r == ABSENT_CODE) {
            absent_hits[absent++] = i;
        } else {
            failed_hits[failed++] = i;
        }
    }

    /* Three verdicts, and the PASS line is no longer a lie about coverage.
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
     */
    if (failed)
        print_named("FAIL", failed_hits, failed);
    if (absent)
        print_named("ABSENT", absent_hits, absent);
    if (skipped)
        print_named("SKIP", skipped_hits, skipped);

    if (failed || absent) {
        printf("NETWORK_SUITE: FAIL (%d passed, %d skipped, %d absent, "
               "%d failed)\n", passed, skipped, absent, failed);
        return 1;
    }
    if (skipped) {
        printf("NETWORK_SUITE: PASS (%d passed, %d skipped; coverage "
               "incomplete)\n", passed, skipped);
        return SKIP_CODE;
    }
    printf("NETWORK_SUITE: PASS (%d passed)\n", passed);
    return 0;
}
