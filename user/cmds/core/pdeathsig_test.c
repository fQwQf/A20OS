/*
 * Guest probe for PR_SET_PDEATHSIG delivery through the exit path.
 *
 * `pdeathsig_test parent` forks a child that arms SIGUSR1 as its parent-death
 * signal and reports readiness, then exits.  The child is reparented and the
 * kernel delivers the signal; its handler prints the marker the checker looks
 * for.  Run with a short `sleep` after the parent so the delivery lands before
 * poweroff.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

static void delivered(int sig)
{
    (void)sig;
    printf("PDEATHSIG: delivered\n");
    _exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    if (strcmp(argv[1], "parent") == 0) {
        int fds[2];
        if (pipe(fds) < 0) {
            printf("PDEATHSIG: FAIL pipe\n");
            return 1;
        }
        pid_t child = fork();
        if (child < 0) {
            printf("PDEATHSIG: FAIL fork\n");
            return 1;
        }
        if (child == 0) {
            signal(SIGUSR1, delivered);
            if (prctl(PR_SET_PDEATHSIG, SIGUSR1) < 0) {
                printf("PDEATHSIG: FAIL arm\n");
                _exit(1);
            }
            char ready = 'R';
            if (write(fds[1], &ready, 1) != 1)
                _exit(2);
            for (;;)
                pause();
        }
        char ready = 0;
        if (read(fds[0], &ready, 1) != 1 || ready != 'R') {
            printf("PDEATHSIG: FAIL readiness\n");
            kill(child, SIGKILL);
            return 1;
        }
        /* Exit while the child is still alive: the kernel hands it to a new
         * parent and delivers SIGUSR1. */
        _exit(0);
    }
    if (strcmp(argv[1], "check") == 0) {
        printf("PDEATHSIG: check done\n");
        return 0;
    }
    return 2;
}
