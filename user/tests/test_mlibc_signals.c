/* Actual Native pthread signal coordination; no CPU affinity or placement. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned ready, stage, finished;
static pthread_t main_thread;
static volatile sig_atomic_t handled, handler_mask_ok;

#define CHECK(test)                                                            \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "MLIBC_SIGNALS: FAIL line=%d errno=%d\n",          \
                    __LINE__, errno);                                          \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
#define OK(call) CHECK((call) == 0)

static sigset_t both(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    return set;
}
static void handler(int sig) {
    sigset_t current;
    if (sig == SIGUSR2 && pthread_equal(pthread_self(), main_thread) &&
        !pthread_sigmask(SIG_SETMASK, NULL, &current) &&
        sigismember(&current, SIGUSR2) == 1)
        handler_mask_ok = 1;
    handled++;
}
static void wait_stage(unsigned desired) {
    OK(pthread_mutex_lock(&lock));
    while (stage < desired)
        OK(pthread_cond_wait(&changed, &lock));
    OK(pthread_mutex_unlock(&lock));
}
static void *worker(void *argument) {
    unsigned index = (uintptr_t)argument;
    sigset_t set = both(), old, pending;
    OK(pthread_sigmask(SIG_SETMASK, NULL, &old));
    CHECK(sigismember(&old, SIGUSR1) == 1 && sigismember(&old, SIGUSR2) == 1);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    OK(pthread_sigmask(SIG_UNBLOCK, &one, &old));
    CHECK(sigismember(&old, SIGUSR2) == 1);
    OK(pthread_sigmask(SIG_SETMASK, &set, NULL));
    OK(pthread_mutex_lock(&lock));
    ready++;
    OK(pthread_cond_broadcast(&changed));
    OK(pthread_mutex_unlock(&lock));
    wait_stage(1);
    /* Repeated peeks must leave the target's signal pending. Another thread
     * must never observe or consume the same thread-directed notification. */
    for (unsigned repeat = 0; repeat < 2; repeat++) {
        CHECK(sigpending(&pending) == 0);
        CHECK(sigismember(&pending, SIGUSR1) == (index == 0));
    }
    struct timespec zero = {0};
    siginfo_t info;
    int sig = sigtimedwait(&set, &info, &zero);
    CHECK(index ? (sig == -1 && errno == EAGAIN)
                : (sig == SIGUSR1 && info.si_signo == SIGUSR1));
    CHECK(sigtimedwait(&set, NULL, &zero) == -1 && errno == EAGAIN);
    OK(pthread_mutex_lock(&lock));
    finished++;
    OK(pthread_cond_broadcast(&changed));
    OK(pthread_mutex_unlock(&lock));
    wait_stage(2);
    struct timespec limit = {.tv_sec = 3};
    if (index) {
        int received;
        OK(sigwait(&set, &received));
        CHECK(received == SIGUSR1);
    } else {
        CHECK(sigtimedwait(&set, NULL, &limit) == SIGUSR2);
    }
    wait_stage(3);
    if (!index)
        OK(pthread_kill(main_thread, SIGUSR2));
    return (void *)(uintptr_t)(20 + index);
}
int main(void) {
    main_thread = pthread_self();
    sigset_t set = both(), old, pending;
    OK(pthread_sigmask(SIG_SETMASK, &set, NULL));
    struct sigaction action = {.sa_handler = handler};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR2, &action, NULL) == 0);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        OK(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    OK(pthread_mutex_lock(&lock));
    while (ready != 2)
        OK(pthread_cond_wait(&changed, &lock));
    OK(pthread_kill(threads[0], 0));
    CHECK(pthread_kill(threads[0], 64) == EINVAL);
    OK(pthread_kill(threads[0], SIGUSR1));
    stage = 1;
    OK(pthread_cond_broadcast(&changed));
    while (finished != 2)
        OK(pthread_cond_wait(&changed, &lock));
    OK(pthread_sigmask(SIG_SETMASK, NULL, &old));
    CHECK(sigismember(&old, SIGUSR1) == 1 && sigismember(&old, SIGUSR2) == 1);
    CHECK(sigpending(&pending) == 0 && !sigismember(&pending, SIGUSR1));
    OK(pthread_kill(threads[0], SIGUSR2));
    OK(pthread_kill(threads[1], SIGUSR1));
    stage = 3;
    OK(pthread_cond_broadcast(&changed));
    OK(pthread_mutex_unlock(&lock));
    sigdelset(&set, SIGUSR2);
    CHECK(sigsuspend(&set) == -1 && errno == EINTR);
    CHECK(handled == 1 && handler_mask_ok);
    OK(pthread_sigmask(SIG_SETMASK, NULL, &old));
    CHECK(sigismember(&old, SIGUSR2) == 1);
    for (unsigned i = 0; i < 2; i++) {
        void *value;
        OK(pthread_join(threads[i], &value));
        CHECK(value == (void *)(uintptr_t)(20 + i));
    }
    /* A cloned process receives a new real identity and no parent thread
     * capabilities. Existing fork()/waitpid() callers must keep working. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        main_thread = pthread_self();
        OK(pthread_kill(main_thread, 0));
        OK(pthread_kill(main_thread, SIGUSR2));
        sigset_t child_set = both();
        struct timespec zero = {0};
        CHECK(sigtimedwait(&child_set, NULL, &zero) == SIGUSR2);
        _exit(0);
    }
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    struct timespec short_wait = {.tv_nsec = 3000000}, before, after;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    CHECK(sigtimedwait(&set, NULL, &short_wait) == -1 && errno == EAGAIN);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    int64_t elapsed = (after.tv_sec - before.tv_sec) * INT64_C(1000000000) +
                      after.tv_nsec - before.tv_nsec;
    CHECK(elapsed >= 3000000);
    short_wait.tv_nsec = 1000000000;
    CHECK(sigtimedwait(&set, NULL, &short_wait) == -1 && errno == EINVAL);
    CHECK(pthread_sigmask(999, &set, NULL) == EINVAL);
    puts("MLIBC_SIGNALS: PASS target masks pending wait timeout suspend join");
    return 0;
}
