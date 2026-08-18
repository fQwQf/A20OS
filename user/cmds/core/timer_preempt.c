#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define HOG_DURATION_NS   750000000ULL
#define SLEEP_DURATION_NS 100000000ULL

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int write_marker(int fd, char marker)
{
    ssize_t n;
    do {
        n = write(fd, &marker, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

static void stop_child(pid_t pid)
{
    if (pid > 0)
        (void)kill(pid, SIGKILL);
}

static void reap_child(pid_t pid)
{
    if (pid <= 0)
        return;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("TIMER_PREEMPT: start\n");

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        printf("TIMER_PREEMPT: FAIL pipe errno=%d\n", errno);
        return 1;
    }

    uint64_t started = now_ns();
    if (!started) {
        printf("TIMER_PREEMPT: FAIL clock\n");
        return 1;
    }

    pid_t hog = fork();
    if (hog == 0) {
        close(pipefd[0]);
        uint64_t begin = now_ns();
        if (!begin)
            _exit(2);
        while (now_ns() - begin < HOG_DURATION_NS)
            ;
        int rc = write_marker(pipefd[1], 'H');
        close(pipefd[1]);
        _exit(rc == 0 ? 0 : 3);
    }
    if (hog < 0) {
        printf("TIMER_PREEMPT: FAIL fork-hog errno=%d\n", errno);
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    pid_t sleeper = fork();
    if (sleeper == 0) {
        close(pipefd[0]);
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = SLEEP_DURATION_NS,
        };
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
        int rc = write_marker(pipefd[1], 'S');
        close(pipefd[1]);
        _exit(rc == 0 ? 0 : 4);
    }
    if (sleeper < 0) {
        printf("TIMER_PREEMPT: FAIL fork-sleeper errno=%d\n", errno);
        close(pipefd[0]);
        close(pipefd[1]);
        stop_child(hog);
        reap_child(hog);
        return 1;
    }

    close(pipefd[1]);
    char order[3] = {0};
    ssize_t first;
    do {
        first = read(pipefd[0], &order[0], 1);
    } while (first < 0 && errno == EINTR);
    uint64_t first_ns = now_ns() - started;

    if (first != 1 || order[0] != 'S') {
        printf("TIMER_PREEMPT: order=%c first_ms=%llu\n",
               first == 1 ? order[0] : '?',
               (unsigned long long)(first_ns / 1000000ULL));
        printf("TIMER_PREEMPT: FAIL sleeper-did-not-preempt-hog\n");
        close(pipefd[0]);
        stop_child(hog);
        stop_child(sleeper);
        reap_child(hog);
        reap_child(sleeper);
        return 1;
    }

    ssize_t second;
    do {
        second = read(pipefd[0], &order[1], 1);
    } while (second < 0 && errno == EINTR);
    close(pipefd[0]);

    int hog_status = 0;
    int sleeper_status = 0;
    int hog_wait = waitpid(hog, &hog_status, 0);
    int sleeper_wait = waitpid(sleeper, &sleeper_status, 0);
    uint64_t total_ns = now_ns() - started;

    int passed = second == 1 && order[1] == 'H' &&
                 hog_wait == hog && sleeper_wait == sleeper &&
                 WIFEXITED(hog_status) && WEXITSTATUS(hog_status) == 0 &&
                 WIFEXITED(sleeper_status) &&
                 WEXITSTATUS(sleeper_status) == 0;
    printf("TIMER_PREEMPT: order=%s first_ms=%llu total_ms=%llu\n",
           order,
           (unsigned long long)(first_ns / 1000000ULL),
           (unsigned long long)(total_ns / 1000000ULL));
    printf("TIMER_PREEMPT: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
