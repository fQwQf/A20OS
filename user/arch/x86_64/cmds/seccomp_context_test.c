#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

struct sock_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};
struct sock_fprog {
    uint16_t len;
    struct sock_filter *filter;
};
static __thread int expected, tid, received, errors;
static int ready, start;

static void handler(int sig, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (sig != SIGSYS || info->si_code != 1 || info->si_errno != expected ||
        info->si_syscall != SYS_getppid || info->si_arch != 0xc000003eU ||
        (uintptr_t)info->si_call_addr !=
            (uintptr_t)uc->uc_mcontext.gregs[REG_RIP] ||
        uc->uc_mcontext.gregs[REG_RAX] != SYS_getppid ||
        syscall(SYS_gettid) != tid)
        errors++;
    received++;
    uc->uc_mcontext.gregs[REG_RAX] = 777 + expected;
}

static void *worker(void *arg) {
    expected = (int)(uintptr_t)arg + 1;
    tid = syscall(SYS_gettid);
    struct sock_filter insns[] = {
        {0x20, 0, 0, 0},
        {0x15, 0, 1, SYS_getppid},
        {0x06, 0, 0, 0x00030000U | (unsigned)expected},
        {0x06, 0, 0, 0x7fff0000U},
    };
    struct sock_fprog prog = {4, insns};
    int failed = prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
                 prctl(PR_SET_SECCOMP, 2L, &prog, 0L, 0L);
    __atomic_add_fetch(&ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&start, __ATOMIC_ACQUIRE))
        usleep(1000);
    if (failed)
        return (void *)1;
    for (int i = 0; i < 200; i++)
        if (syscall(SYS_getppid) != 777 + expected)
            errors++;
    return (void *)(uintptr_t)(errors || received != 200);
}

int main(void) {
    struct sigaction act = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGSYS, &act, NULL))
        return 1;
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++)
        if (pthread_create(&threads[i], NULL, worker, (void *)i))
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
    puts(failed ? "SECCOMP_CONTEXT: FAIL"
                : "SECCOMP_CONTEXT: PASS thread+siginfo+registers+emulation");
    return failed;
}
