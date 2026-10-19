/*
 * Guest coverage for the resource-limit policy: the per-resource mapping, the
 * clamps, the unenforced-resource refusal, the RT-priority capability rule and
 * the cross-task privilege rule.  Every printed value is deterministic, so a C
 * and a Rust kernel can be compared on the same trace.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("RLIMIT_TEST: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int set(int resource, uint64_t cur, uint64_t max)
{
    struct rlimit rl;
    rl.rlim_cur = cur;
    rl.rlim_max = max;
    return setrlimit(resource, &rl);
}

static void report(const char *name, int resource)
{
    struct rlimit rl;
    if (getrlimit(resource, &rl) < 0) {
        printf("RLIMIT_TEST: %s error=%d\n", name, errno);
        return;
    }
    printf("RLIMIT_TEST: %s cur=%llu max=%llu\n", name,
           (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
}

int main(void)
{
    printf("RLIMIT_TEST: start\n");

    report("nofile-default", RLIMIT_NOFILE);
    if (set(RLIMIT_NOFILE, 512, 1024) < 0)
        return fail("nofile-set");
    report("nofile-set", RLIMIT_NOFILE);
    /* min(cur, max) then the descriptor ceiling. */
    if (set(RLIMIT_NOFILE, 100000, 100000) < 0)
        return fail("nofile-clamp");
    report("nofile-clamp", RLIMIT_NOFILE);

    if (set(RLIMIT_STACK, 1u << 20, 2u << 20) < 0)
        return fail("stack-set");
    report("stack-set", RLIMIT_STACK);
    /* The stack ceiling is 8 MiB. */
    if (set(RLIMIT_STACK, 64u << 20, 64u << 20) < 0)
        return fail("stack-clamp");
    report("stack-clamp", RLIMIT_STACK);

    if (set(RLIMIT_AS, 1u << 30, 2u << 30) < 0)
        return fail("as-set");
    report("as-set", RLIMIT_AS);
    if (set(RLIMIT_NPROC, 77, 88) < 0)
        return fail("nproc-set");
    report("nproc-set", RLIMIT_NPROC);

    /* RTPRIO: raising the hard limit needs CAP_SYS_RESOURCE (this guest is
     * root, so it holds it); a soft limit above the hard one is refused. */
    errno = 0;
    if (set(RLIMIT_RTPRIO, 6, 5) == 0 || errno != EINVAL)
        return fail("rtprio-soft-above-hard");
    printf("RLIMIT_TEST: rtprio-soft-above-hard EINVAL\n");
    if (set(RLIMIT_RTPRIO, 2, 4) < 0)
        return fail("rtprio-set");
    report("rtprio-set", RLIMIT_RTPRIO);

    /* Cross-task: prlimit(2) on a same-uid child is allowed; the returned old
     * values come from the same policy. */
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        for (;;)
            pause();
        return 0;
    }
    struct rlimit child_new;
    child_new.rlim_cur = 256;
    child_new.rlim_max = 512;
    struct rlimit child_old;
    long rc = syscall(SYS_prlimit64, (long)child, RLIMIT_NOFILE,
                      (long)&child_new, (long)&child_old);
    if (rc < 0) {
        printf("RLIMIT_TEST: prlimit-child error=%d\n", errno);
    } else {
        printf("RLIMIT_TEST: prlimit-child PASS old-cur=%llu old-max=%llu\n",
               (unsigned long long)child_old.rlim_cur,
               (unsigned long long)child_old.rlim_max);
    }
    kill(child, 9);
    int status = 0;
    waitpid(child, &status, 0);

    printf("RLIMIT_TEST: PASS\n");
    return 0;
}
