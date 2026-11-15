/* A reported process stop must freeze every thread before the parent sees it. */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shared { uint32_t count[2], ready; };
static struct shared *shared;
static uint64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void *worker(void *arg)
{
    unsigned index = (unsigned)(uintptr_t)arg;
    __atomic_fetch_add(&shared->ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        __atomic_fetch_add(&shared->count[index], 1, __ATOMIC_RELAXED);
        sched_yield();
    }
    return NULL;
}
static int wait_status(pid_t child, int flags, int *status)
{
    uint64_t end = now_ms() + 3000;
    while (now_ms() < end) {
        pid_t result = waitpid(child, status, WNOHANG | flags);
        if (result == child) return 0;
        if (result < 0) return -1;
        usleep(1000);
    }
    return -1;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, (void *)1)) _exit(2);
        worker(NULL);
        _exit(3);
    }
    int failed = 0, status = 0;
    unsigned round = 0;
    uint64_t end = now_ms() + 3000;
    while (__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE) != 2 && now_ms() < end)
        usleep(1000);
    if (__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE) != 2) failed = 1;
    for (; !failed && round < 20; round++) {
        if (kill(child, SIGSTOP) || wait_status(child, WUNTRACED, &status) ||
            !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) { failed = 1; break; }
        uint32_t before[2];
        for (unsigned i = 0; i < 2; i++) before[i] = __atomic_load_n(&shared->count[i], __ATOMIC_ACQUIRE);
        usleep(100000);
        for (unsigned i = 0; i < 2; i++)
            if (before[i] != __atomic_load_n(&shared->count[i], __ATOMIC_ACQUIRE)) failed = 1;
        if (failed) break;
        if (kill(child, SIGCONT) || wait_status(child, WCONTINUED, &status) ||
            !WIFCONTINUED(status)) { failed = 1; break; }
        end = now_ms() + 3000;
        for (;;) {
            int progress = 1;
            for (unsigned i = 0; i < 2; i++)
                if (before[i] == __atomic_load_n(&shared->count[i], __ATOMIC_ACQUIRE)) progress = 0;
            if (progress) break;
            if (now_ms() >= end) { failed = 1; break; }
            usleep(1000);
        }
    }
    if (kill(child, SIGKILL) || wait_status(child, 0, &status) ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) failed = 1;
    printf("SIGSTOP_GROUP: rounds=%u counters_frozen=%u %s\n", round, round, failed ? "FAIL" : "PASS");
    munmap(shared, 4096);
    return failed;
}
