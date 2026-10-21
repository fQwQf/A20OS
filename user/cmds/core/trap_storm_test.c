#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef CONFIG_TRAP_STORM_TEST
#include "uapi/a20/trap_storm.h"
#endif

/*
 * Guest-side acceptance for the trap-storm diagnostic (kernel/core/trap.c).
 *
 *   --inject  requires the TRAP_STORM_TEST build: a fault at
 *             A20_TRAP_STORM_TEST_VA is reported as repaired without changing
 *             the mapping, so one store instruction refaults at the same
 *             pc/va/cause with no signal and no syscall in between.  The
 *             detector must log [TRAP-STORM] and SIGKILL that task.
 *   (default) the same checks minus the injection, for a normal build.
 *
 * Always run:
 *   - "pager": demand-pages and COW-copies anonymous memory in a loop.  It
 *     must finish normally, proving ordinary fault traffic is not a storm.
 *   - "sigsegv": stores through an ordinary unmapped address with no handler.
 *     It must die from SIGSEGV, not SIGKILL, proving the detector did not
 *     mistake a plain fault for a storm.
 *   - "worker": burns CPU for a fixed wall-clock budget while the storm task
 *     runs, and must finish normally -- the unrelated-progress half.
 *
 * The [TRAP-STORM] record itself is asserted by the smoke case's log
 * expectations; a guest program cannot read the kernel log.
 */

#define STORM_WAIT_MS 8000
#define WORKER_BUDGET_US 1200000

static int worker_child(void) {
    struct timeval start, now;
    gettimeofday(&start, NULL);
    unsigned long acc = 0;
    unsigned long iterations = 0;
    for (;;) {
        for (int i = 0; i < 4096; i++)
            acc = acc * 1103515245UL + 12345UL;
        iterations++;
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000L +
                       (now.tv_usec - start.tv_usec);
        if (elapsed >= WORKER_BUDGET_US)
            break;
    }
    if (acc == 0 && iterations == 0)
        return 101;
    return 0;
}

static int pager_child(void) {
    enum { PAGES = 64 };
    for (int round = 0; round < 8; round++) {
        unsigned char *region = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED)
            return 102;
        for (int page = 0; page < PAGES; page++)
            region[page * 4096] = (unsigned char)(round + page);
        pid_t child = fork();
        if (child < 0)
            return 103;
        if (child == 0) {
            /* COW-copy every page, then touch the originals again. */
            for (int page = 0; page < PAGES; page++)
                region[page * 4096]++;
            _exit(0);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            return 104;
        for (int page = 0; page < PAGES; page++)
            region[page * 4096]++;
        munmap(region, PAGES * 4096);
    }
    return 0;
}

static int sigsegv_child(void) {
    /* No handler: the fault is fatal, and must be reported as SIGSEGV rather
     * than swallowed by the storm path. */
    volatile unsigned char *p = (volatile unsigned char *)0x21000000UL;
    for (;;)
        *p = 1;
}

static int reap_until(pid_t pid, int *status, int budget_ms) {
    for (int waited = 0; waited < budget_ms; waited++) {
        pid_t got = waitpid(pid, status, WNOHANG);
        if (got == pid)
            return 0;
        usleep(1000);
    }
    return -1;
}

int main(int argc, char **argv) {
    int inject = argc > 1 && strcmp(argv[1], "--inject") == 0;
    int failures = 0;

    /* Ordinary demand paging and COW must not look like a storm. */
    pid_t pager = fork();
    if (pager == 0)
        _exit(pager_child());
    if (pager < 0) {
        printf("TRAP_STORM: FAIL spawn-pager errno=%d\n", errno);
        return 1;
    }
    int pager_status = 0;
    if (waitpid(pager, &pager_status, 0) != pager ||
        !WIFEXITED(pager_status) || WEXITSTATUS(pager_status) != 0) {
        printf("TRAP_STORM: FAIL pager status=%d\n", pager_status);
        failures++;
    } else {
        printf("TRAP_STORM: demand-cow-clean PASS\n");
    }

    /* An ordinary fatal fault must stay a SIGSEGV, not become a storm kill. */
    pid_t plain = fork();
    if (plain == 0)
        _exit(sigsegv_child());
    if (plain < 0) {
        printf("TRAP_STORM: FAIL spawn-sigsegv errno=%d\n", errno);
        return 1;
    }
    int plain_status = 0;
    if (waitpid(plain, &plain_status, 0) != plain ||
        !WIFSIGNALED(plain_status) || WTERMSIG(plain_status) != SIGSEGV) {
        printf("TRAP_STORM: FAIL plain-fault status=%d\n", plain_status);
        failures++;
    } else {
        printf("TRAP_STORM: plain-fault-signalled PASS\n");
    }

    if (!inject) {
        if (failures)
            return 1;
        printf("TRAP_STORM: PASS\n");
        return 0;
    }

#ifdef CONFIG_TRAP_STORM_TEST
    pid_t storm = fork();
    if (storm == 0) {
        volatile unsigned char *p =
            (volatile unsigned char *)A20_TRAP_STORM_TEST_VA;
        for (;;)
            *p = 1;
    }
    pid_t worker = fork();
    if (worker == 0)
        _exit(worker_child());
    if (storm < 0 || worker < 0) {
        printf("TRAP_STORM: FAIL fork errno=%d\n", errno);
        return 1;
    }

    int storm_status = 0, worker_status = 0;
    if (reap_until(storm, &storm_status, STORM_WAIT_MS) != 0) {
        printf("TRAP_STORM: FAIL refault loop never terminated\n");
        kill(storm, SIGKILL);
        waitpid(storm, &storm_status, 0);
        failures++;
    } else if (!WIFSIGNALED(storm_status) || WTERMSIG(storm_status) != SIGKILL) {
        printf("TRAP_STORM: FAIL storm status=%d\n", storm_status);
        failures++;
    } else {
        printf("TRAP_STORM: refault-terminated PASS\n");
    }

    if (waitpid(worker, &worker_status, 0) != worker ||
        !WIFEXITED(worker_status) || WEXITSTATUS(worker_status) != 0) {
        printf("TRAP_STORM: FAIL unrelated-worker status=%d\n", worker_status);
        failures++;
    } else {
        printf("TRAP_STORM: unrelated-progress PASS\n");
    }
#else
    printf("TRAP_STORM: FAIL --inject needs a TRAP_STORM_TEST build\n");
    return 1;
#endif

    if (failures)
        return 1;
    printf("TRAP_STORM: PASS\n");
    return 0;
}
