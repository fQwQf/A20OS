#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static __thread sigjmp_buf recovery;
static __thread void *guard;
static __thread int tid;
static __thread volatile sig_atomic_t received, errors;
static __thread int expected_code = SEGV_ACCERR;
static __thread uintptr_t observed_addr;
static __thread int observed_tid, observed_code;
static int ready, start;

static void fault_handler(int sig, siginfo_t *info, void *context) {
    (void)context;
    if (sig != SIGSEGV || info->si_code != expected_code ||
        info->si_addr != guard || syscall(SYS_gettid) != tid) {
        errors++;
        observed_addr = (uintptr_t)info->si_addr;
        observed_tid = syscall(SYS_gettid);
        observed_code = info->si_code;
    }
    received++;
    siglongjmp(recovery, 1);
}

static void *worker(void *arg) {
    (void)arg;
    tid = syscall(SYS_gettid);
    guard = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE))
        usleep(1000);
    if (guard == MAP_FAILED)
        return (void *)1;
    /* Exercise both untouched reservations and already populated leaves. */
    for (int populated = 0; populated < 2; populated++) {
        if (populated) {
            if (mprotect(guard, 4096, PROT_READ | PROT_WRITE))
                return (void *)1;
            *(volatile int *)guard = 0x12345678;
            if (mprotect(guard, 4096, PROT_NONE))
                return (void *)1;
        }
        for (int write = 0; write < 2; write++) {
            for (int i = 0; i < 200; i++) {
                if (!sigsetjmp(recovery, 1)) {
                    if (write)
                        *(volatile int *)guard = 7;
                    else {
                        volatile int value = *(volatile int *)guard;
                        (void)value;
                    }
                    errors++;
                }
            }
        }
        if (populated) {
            if (mprotect(guard, 4096, PROT_READ | PROT_WRITE) ||
                *(volatile int *)guard != 0x12345678)
                errors++;
        }
    }
    munmap(guard, 4096);
    if (errors || received != 800)
        printf("SIGNAL_FAULT: tid=%d received=%d errors=%d addr=%lx "
               "expected=%lx code=%d handler_tid=%d\n",
               tid, received, errors, (unsigned long)observed_addr,
               (unsigned long)guard, observed_code, observed_tid);
    return (void *)(uintptr_t)(errors || received != 800);
}

int main(void) {
    struct sigaction act = {.sa_sigaction = fault_handler,
                            .sa_flags = SA_SIGINFO};
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGSEGV, &act, NULL))
        return 1;
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        if (pthread_create(&threads[i], NULL, worker, NULL))
            return 1;
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 4)
        usleep(1000);
    __atomic_store_n(&start, 1, __ATOMIC_RELEASE);
    int failed = 0;
    for (int i = 0; i < 4; i++) {
        void *result;
        if (pthread_join(threads[i], &result) || result)
            failed = 1;
    }
    /* Protected populated pages still own their data across fork and COW. */
    guard = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (guard == MAP_FAILED)
        return 1;
    *(volatile int *)guard = 0x12345678;
    if (mprotect(guard, 4096, PROT_NONE))
        return 1;
    pid_t child = fork();
    if (!child) {
        if (mprotect(guard, 4096, PROT_READ | PROT_WRITE) ||
            *(volatile int *)guard != 0x12345678)
            _exit(1);
        *(volatile int *)guard = 42;
        _exit(0);
    }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child || status ||
        mprotect(guard, 4096, PROT_READ | PROT_WRITE) ||
        *(volatile int *)guard != 0x12345678)
        failed = 1;
    munmap(guard, 4096);
    tid = syscall(SYS_gettid);
    expected_code = SEGV_MAPERR;
    if (!sigsetjmp(recovery, 1)) {
        volatile int value = *(volatile int *)guard;
        (void)value;
        errors++;
    }
    if (received != 1 || errors)
        failed = 1;
    puts(failed ? "SIGNAL_FAULT: FAIL"
                : "SIGNAL_FAULT: PASS thread+address+context+protection");
    return failed;
}
