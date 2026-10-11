/* Regression for kernel copyout after COW, concurrent adjacent anonymous
 * faults, and asynchronous signal stack preservation.  A successful read()
 * must be visible through the user VA immediately, while the child retains
 * its original private page.  The fault race uses a mapping larger than the
 * anonymous provisioning limit, so first touches race the fallback window. */
#define _GNU_SOURCE
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
/* Signal delivery must preserve ABI-owned stack bytes below the interrupted
 * SP.  Kernel-specific register knowledge belongs to arch; this user ABI
 * probe deliberately exercises the machine calling convention. */
#ifdef __x86_64__
static void handler(int sig) {
    (void)sig;
}
extern int redzone_probe(long pid);
__asm__(".text\n.global redzone_probe\nredzone_probe:\n"
        "mov %rdi,%r8\nlea -128(%rsp),%rdi\nmov $16,%ecx\nmov "
        "$0x12345678,%eax\nrep stosq\n"
        "mov %r8,%rdi\nmov $10,%esi\nmov $62,%eax\nsyscall\n"
        "lea -128(%rsp),%rdi\nmov $16,%ecx\nmov $0x12345678,%eax\nrepe scasq\n"
        "sete %al\nmovzbl %al,%eax\nret\n");
static int signal_probe(void) {
    struct sigaction a = {.sa_handler = handler};
    sigemptyset(&a.sa_mask);
    if (sigaction(SIGUSR1, &a, 0))
        return 1;
    int ok = redzone_probe(getpid());
    printf("VM_FAULT: redzone %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
#else
static int signal_probe(void) {
    return 0;
}
#endif

static volatile uint64_t *area;
static unsigned stage, done[4];
static int errors;
#define ROUNDS 4096
static void spin_until(unsigned *p, unsigned n) {
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) != n)
        sched_yield();
}
static void *worker(void *v) {
    unsigned id = (uintptr_t)v;
    for (unsigned n = 1; n <= ROUNDS; n++) {
        spin_until(&stage, n);
        volatile uint64_t *p = area + ((n - 1) * 4 + id) * 512;
        for (unsigned i = 0; i < 512; i++)
            p[i] = 0xabc0000000UL + (id << 16) + i;
        __atomic_store_n(&done[id], n, __ATOMIC_RELEASE);
    }
    return NULL;
}
static int cow_copy_probe(void) {
    volatile uint64_t *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int data[2], hold[2];
    if (pipe(data) || pipe(hold))
        return 1;
    uint64_t x = 0xdecafbad12345678UL;
    if (p == MAP_FAILED)
        return 1;
    *p = 0x1234;
    if (write(data[1], &x, sizeof(x)) != sizeof(x))
        return 1;
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        char c;
        read(hold[0], &c, 1);
        _exit(*p != 0x1234);
    }
    uint64_t before = *p;
    int r = read(data[0], (void *)p, sizeof(x));
    uint64_t after = *p;
    write(hold[1], "x", 1);
    int st;
    waitpid(pid, &st, 0);
    printf("VM_FAULT: cow-copy %s before=%llx after=%llx bytes=%d child=%d\n",
           r == sizeof(x) && after == x && st == 0 ? "PASS" : "FAIL",
           (unsigned long long)before, (unsigned long long)after, r, st);
    fflush(stdout);
    munmap((void *)p, 4096);
    close(data[0]);
    close(data[1]);
    close(hold[0]);
    close(hold[1]);
    return r != sizeof(x) || after != x || st != 0;
}

static int *futex_word;
static unsigned futex_ready;
static void *futex_waiter(void *unused) {
    (void)unused;
    struct timespec limit = {.tv_sec = 2};
    __atomic_store_n(&futex_ready, 1, __ATOMIC_RELEASE);
    long r =
        syscall(SYS_futex, futex_word, 128 /* WAIT_PRIVATE */, 0, &limit, 0, 0);
    return (void *)(uintptr_t)(r != 0);
}

static int futex_cow_probe(void) {
    int failed = 0;
    for (unsigned i = 0; i < 4; i++) {
        futex_word = mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (futex_word == MAP_FAILED)
            return 1;
        *futex_word = 0;
        futex_ready = 0;
        pthread_t thread;
        if (pthread_create(&thread, 0, futex_waiter, 0))
            return 1;
        spin_until(&futex_ready, 1);
        /* Let the waiter enter the kernel before making its page COW. */
        usleep(50000);
        int hold[2];
        if (pipe(hold))
            return 1;
        pid_t child = fork();
        if (child < 0)
            return 1;
        if (!child) {
            char c;
            read(hold[0], &c, 1);
            _exit(*futex_word != 0);
        }
        __atomic_store_n(futex_word, 1, __ATOMIC_RELEASE);
        long woke =
            syscall(SYS_futex, futex_word, 129 /* WAKE_PRIVATE */, 1, 0, 0, 0);
        void *result;
        pthread_join(thread, &result);
        write(hold[1], "x", 1);
        int status;
        waitpid(child, &status, 0);
        failed |= woke != 1 || result != 0 || status != 0;
        close(hold[0]);
        close(hold[1]);
        munmap(futex_word, 4096);
    }
    printf("VM_FAULT: futex-cow %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
static int sparse_unmap_probe(void) {
    const size_t span = (size_t)(8ULL * 1024 * 1024 * 1024);
    if (!span)
        return 0;
    char *p = mmap(0, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int failed = munmap(p, span) != 0;
    clock_gettime(CLOCK_MONOTONIC, &end);
    long ms = (end.tv_sec - start.tv_sec) * 1000 +
              (end.tv_nsec - start.tv_nsec) / 1000000;
    printf("VM_FAULT: sparse-unmap %s ms=%ld\n", failed ? "FAIL" : "PASS", ms);
    return failed;
}
int main(void) {
    int failed = sparse_unmap_probe();
    failed |= signal_probe();
    failed |= cow_copy_probe();
    failed |= futex_cow_probe();
    fflush(stdout);
    area = mmap(0, 64UL * 1024 * 1024, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED)
        return 3;
    pthread_t t[4];
    for (uintptr_t i = 0; i < 4; i++)
        if (pthread_create(&t[i], 0, worker, (void *)i))
            return 2;
    puts("VM_FAULT: anon-race START");
    fflush(stdout);
    for (unsigned n = 1; n <= ROUNDS; n++) {
        __atomic_store_n(&stage, n, __ATOMIC_RELEASE);
        for (unsigned id = 0; id < 4; id++)
            spin_until(&done[id], n);
        for (unsigned id = 0; id < 4; id++) {
            volatile uint64_t *p = area + ((n - 1) * 4 + id) * 512;
            for (unsigned i = 0; i < 512; i++)
                if (p[i] != 0xabc0000000UL + (id << 16) + i) {
                    errors++;
                    break;
                }
        }
    }
    for (unsigned i = 0; i < 4; i++)
        pthread_join(t[i], 0);
    printf("VM_FAULT: anon-race %s errors=%d\n", errors ? "FAIL" : "PASS",
           errors);
    fflush(stdout);
    munmap((void *)area, 64UL * 1024 * 1024);
    failed |= errors != 0;
    printf("VM_FAULT: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
