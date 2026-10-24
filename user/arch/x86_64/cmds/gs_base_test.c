#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET_GS 0x1001
#define GET_GS 0x1004
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "GS_BASE: FAIL line=%d errno=%d\n", __LINE__,      \
                    errno);                                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static uint64_t gs_read(void) {
    uint64_t v;
    __asm__ volatile("movq %%gs:0,%0" : "=r"(v));
    return v;
}
static void gs_write(uint64_t v) {
    __asm__ volatile("movq %0,%%gs:0" ::"r"(v) : "memory");
}
static int exercise(uint64_t token) {
    uint64_t *base = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED);
    CHECK(syscall(SYS_arch_prctl, SET_GS, base) == 0);
    /* First GS access faults in an absent page: entry must distinguish user
     * GS from the kernel task pointer even on a demand-paging exception. */
    gs_write(token);
    CHECK(*base == token);
    for (int i = 0; i < 10000; i++) {
        CHECK(gs_read() == token);
        if (!(i % 64)) {
            CHECK(getpid() > 0);
            sched_yield();
        }
    }
    uint64_t seen = 0;
    CHECK(syscall(SYS_arch_prctl, GET_GS, &seen) == 0 &&
          seen == (uintptr_t)base);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        if (gs_read() != token)
            _exit(1);
        gs_write(token + 1);
        if (gs_read() != token + 1)
            _exit(2);
        _exit(0);
    }
    int st;
    CHECK(waitpid(child, &st, 0) == child && WIFEXITED(st) && !WEXITSTATUS(st));
    CHECK(gs_read() == token);
    CHECK(syscall(SYS_arch_prctl, SET_GS, 0) == 0);
    CHECK(munmap(base, 4096) == 0);
    return 0;
}
static void *thread(void *arg) {
    return (void *)(uintptr_t)exercise((uintptr_t)arg);
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        uint64_t base = 1;
        return syscall(SYS_arch_prctl, GET_GS, &base) < 0 || base != 0;
    }
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++)
        CHECK(pthread_create(&threads[i], NULL, thread, (void *)(0xa200 + i)) ==
              0);
    for (int i = 0; i < 4; i++) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && !result);
    }
    /* musl's raw syscall() returns the kernel's -EPERM encoding (-1) without
     * touching errno, so the assertion checks the return value only. */
    CHECK(syscall(SYS_arch_prctl, SET_GS, 0xffff800000000000ULL) == -1);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        uint64_t base = 42;
        if (syscall(SYS_arch_prctl, SET_GS, &base) < 0)
            _exit(1);
        char *args[] = {"gs_base_test", "exec", NULL};
        execv("/bin/gs_base_test", args);
        perror("GS_BASE exec");
        _exit(2);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st));
    puts("GS_BASE: PASS demand-fault+syscall+preempt+threads+fork+COW+exec");
    return 0;
}
