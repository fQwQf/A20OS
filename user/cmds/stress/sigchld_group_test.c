#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int release_pipe[2];
static int ready_pipe[2];

static void *worker(void *unused) {
    (void)unused;
    char byte = 'R';
    if (write(ready_pipe[1], &byte, 1) != 1 ||
        read(release_pipe[0], &byte, 1) != 1)
        _exit(99);
    syscall(SYS_exit, 23);
    return NULL;
}

static void caught(int signal) { (void)signal; }

static int fail(const char *message) {
    printf("SIGCHLD_GROUP: FAIL %s errno=%d\n", message, errno);
    return 1;
}

int main(void) {
    sigset_t mask, previous;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    /* A caught disposition keeps SIGCHLD observable on kernels which discard
     * default-ignored signals at generation. We consume it synchronously. */
    struct sigaction action = {.sa_handler = caught};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) ||
        sigprocmask(SIG_BLOCK, &mask, &previous) ||
        pipe(release_pipe) || pipe(ready_pipe))
        return fail("setup");
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (!child) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL))
            _exit(98);
        /* Exit only the leader while its sibling remains blocked. */
        syscall(SYS_exit, 23);
        _exit(97);
    }
    char byte;
    if (read(ready_pipe[0], &byte, 1) != 1)
        return fail("worker ready");
    siginfo_t info;
    struct timespec timeout = {.tv_nsec = 200000000};
    errno = 0;
    int early = sigtimedwait(&mask, &info, &timeout);
    int status;
    if (early != -1 || errno != EAGAIN || waitpid(child, &status, WNOHANG) != 0) {
        int result = fail("leader exit must not notify before group completion");
        write(release_pipe[1], "X", 1);
        waitpid(child, NULL, 0);
        return result;
    }
    if (write(release_pipe[1], "X", 1) != 1)
        return fail("release worker");
    timeout = (struct timespec){.tv_sec = 5};
    if (sigtimedwait(&mask, &info, &timeout) != SIGCHLD)
        return fail("last member must notify parent");
    if (info.si_code != CLD_EXITED || info.si_pid != child ||
        info.si_uid != getuid() || info.si_status != 23)
        return fail("SIGCHLD exit siginfo");
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 23)
        return fail("notified group must be waitable");
    sigprocmask(SIG_SETMASK, &previous, NULL);
    close(release_pipe[0]); close(release_pipe[1]);
    close(ready_pipe[0]); close(ready_pipe[1]);
    puts("SIGCHLD_GROUP: PASS");
    return 0;
}
