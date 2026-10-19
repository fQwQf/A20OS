/*
 * Guest fault probe for the exec replacement transaction.
 *
 * The test-profile kernel exposes one private operation (prctl 0xA20E) that
 * fails the current task's next exec at a commit phase Rust declares failable
 * (a20_elf_phase_failable). Adding 0x100 requests injection after a phase;
 * only stack, vdso and new-mm preparation are safe for after-effect failure.
 *
 * For each supported before/after point the probe forks a child that
 *   1. opens a descriptor and starts a sibling thread,
 *   2. arms the injection and execve()s,
 *   3. after the expected failure checks that it is still running, that the
 *      descriptor still works and that the sibling thread is still alive,
 *   4. reports success on a CLOEXEC pipe only after all of that.
 *
 * A successful exec would close the report pipe without a byte, so the parent
 * cannot mistake "exec worked" for "exec failed cleanly".
 *
 * Each failed attempt is interleaved with a successful exec of /bin/true, so a
 * rollback path that left the task unable to exec (or that only works once) is
 * caught immediately; a final successful exec of /bin/echo proves the new
 * image really runs afterwards.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PR_A20_EXEC_FAULT 0xA20E

/* Commit-phase discriminants (kernel/include/mm/elf_rust.h). */
#define PHASE_STACK_BUILD     0
#define PHASE_VDSO_MAP        1
#define PHASE_ALLOC_NEW_MM    2
#define PHASE_FDTABLE_UNSHARE 4
#define PHASE_AFTER 0x100

#define ITERS_PER_PHASE 6

extern char **environ;

static volatile int sibling_alive;

static void *sibling_main(void *arg)
{
    (void)arg;
    __atomic_store_n(&sibling_alive, 1, __ATOMIC_RELEASE);
    for (;;)
        pause();
    return NULL;
}

static int child_fail_attempt(unsigned phase, int report_fd, const char *path)
{
    /* A descriptor that must survive the failed replacement. */
    int kfd[2];
    if (pipe(kfd) != 0)
        return 20;

    __atomic_store_n(&sibling_alive, 0, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, sibling_main, NULL) != 0)
        return 21;
    for (int i = 0; i < 100000 && !__atomic_load_n(&sibling_alive, __ATOMIC_ACQUIRE); i++)
        sched_yield();
    if (!__atomic_load_n(&sibling_alive, __ATOMIC_ACQUIRE))
        return 22;

    if (syscall(SYS_prctl, PR_A20_EXEC_FAULT, (unsigned long)phase,
                0UL, 0UL, 0UL) != 0)
        return 23;

    char *argv[] = { (char *)path, "EXECFAULT-SHOULD-NOT-RUN", NULL };
    execve(path, argv, environ);

    /* execve returned: the replacement must have failed cleanly. */
    if (errno != ENOMEM) {
        fprintf(stderr, "EXECFAULT: exec %s returned errno=%d (%s), expected ENOMEM\n",
                path, errno, strerror(errno));
        return 24;
    }
    if (!__atomic_load_n(&sibling_alive, __ATOMIC_ACQUIRE))
        return 25;                    /* sibling thread was lost */
    struct stat st;
    if (fstat(kfd[0], &st) != 0)
        return 26;                    /* descriptor was lost */
    if (write(kfd[1], "x", 1) != 1)
        return 27;
    char c = 0;
    if (read(kfd[0], &c, 1) != 1 || c != 'x')
        return 28;                    /* descriptor state corrupted */
    close(kfd[0]);
    close(kfd[1]);

    /* Only now, with everything verified, report on the protected pipe. */
    char ok = 'F';
    if (write(report_fd, &ok, 1) != 1)
        return 29;
    return 0;
}

static int run_quiet_exec(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "/bin/true", NULL };
        execve("/bin/true", argv, environ);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int run_phase(unsigned phase, const char *path, int iterations,
                     const char *label)
{
    for (int it = 0; it < iterations; it++) {
        int pfd[2];
        if (pipe(pfd) != 0) {
            printf("EXECFAULT: phase %u FAIL pipe\n", phase);
            return -1;
        }
        fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

        pid_t pid = fork();
        if (pid < 0) {
            printf("EXECFAULT: phase %u FAIL fork\n", phase);
            return -1;
        }
        if (pid == 0) {
            close(pfd[0]);
            int rc = child_fail_attempt(phase, pfd[1], path);
            _exit(rc == 0 ? 0 : rc);
        }

        close(pfd[1]);
        char c = 0;
        ssize_t n = read(pfd[0], &c, 1);
        int status = 0;
        waitpid(pid, &status, 0);
        close(pfd[0]);

        if (!(n == 1 && c == 'F' && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0)) {
            printf("EXECFAULT: phase %u iter %d FAIL n=%zd c=%d "
                   "status=%#x\n", phase, it, n, c, status);
            return -1;
        }

        /* Interleave a successful replacement: a rolled-back exec must not
         * poison the next successful one, and vice versa. */
        if (run_quiet_exec() != 0) {
            printf("EXECFAULT: phase %u iter %d interleaved success FAILED\n",
                   phase, it);
            return -1;
        }
    }
    printf("EXECFAULT: %s phase %u handled (%d injected failures, interleaved)\n",
           label, phase, iterations);
    return 0;
}

static int run_normal_exec(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = { "/bin/echo", "EXECFAULT-NORMAL-OK", NULL };
        execve("/bin/echo", argv, environ);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int main(void)
{
    static const unsigned phases[] = {
        PHASE_STACK_BUILD, PHASE_VDSO_MAP, PHASE_ALLOC_NEW_MM,
        PHASE_FDTABLE_UNSHARE,
        PHASE_STACK_BUILD | PHASE_AFTER,
        PHASE_VDSO_MAP | PHASE_AFTER,
        PHASE_ALLOC_NEW_MM | PHASE_AFTER,
    };
    int failures = 0;

    printf("EXECFAULT: START\n");
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        if (run_phase(phases[i], "/bin/echo", ITERS_PER_PHASE, "linux") != 0)
            failures++;
    }
    /* Native setup stages several capability objects and handle slots before
     * the commit boundary. Fail after stack construction, then prove the
     * original Linux task survives while those reservations stay invisible. */
    if (run_phase(PHASE_STACK_BUILD | PHASE_AFTER,
                  "/bin/native-handle-rv", 2, "native") != 0)
        failures++;
    if (run_normal_exec() != 0) {
        printf("EXECFAULT: normal exec after failures FAILED\n");
        failures++;
    }
    if (failures) {
        printf("EXECFAULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("EXECFAULT: PASS\n");
    return 0;
}
