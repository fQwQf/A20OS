#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    const char *name;
    const char *cmd;
    int may_skip;
} test_case_t;

static const test_case_t tests[] = {
    { "dns_test",          "dns_test localhost",       1 },
    { "tcp_loopback_test", "tcp_loopback_test",        0 },
    { "udp_loopback_test", "udp_loopback_test",        0 },
    { "icmp_loopback_test","icmp_loopback_test",       0 },
    { "unix_test",         "unix_test",                0 },
    { "alg_test",          "alg_test",                 1 },
    { "timeout_test",      "timeout_test",             0 },
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
    if (t->may_skip && WIFEXITED(status) && WEXITSTATUS(status) == 77)
        return 77;
    return 1;
}

int main(void) {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int r = run_test(&tests[i]);
        if (r == 0) {
            passed++;
        } else if (r == 77) {
            skipped++;
        } else {
            failed++;
        }
    }

    if (failed == 0) {
        printf("NETWORK_SUITE: PASS (%d passed, %d skipped)\n", passed, skipped);
        return 0;
    }
    printf("NETWORK_SUITE: FAIL (%d passed, %d skipped, %d failed)\n",
           passed, skipped, failed);
    return 1;
}
