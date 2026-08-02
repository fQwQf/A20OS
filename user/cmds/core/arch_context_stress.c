#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__loongarch64)

static volatile sig_atomic_t handler_seen;

static void load_vr0(const uint64_t value[2])
{
    __asm__ volatile("vld $vr0, %0, 0" : : "r"(value) : "memory");
}

static void store_vr0(uint64_t value[2])
{
    __asm__ volatile("vst $vr0, %0, 0" : : "r"(value) : "memory");
}

static void set_fcc(uint64_t value)
{
    __asm__ volatile(
        "movgr2cf $fcc0, %0\n\t"
        "srli.d $t0, %0, 1\n\tmovgr2cf $fcc1, $t0\n\t"
        "srli.d $t0, %0, 2\n\tmovgr2cf $fcc2, $t0\n\t"
        "srli.d $t0, %0, 3\n\tmovgr2cf $fcc3, $t0\n\t"
        "srli.d $t0, %0, 4\n\tmovgr2cf $fcc4, $t0\n\t"
        "srli.d $t0, %0, 5\n\tmovgr2cf $fcc5, $t0\n\t"
        "srli.d $t0, %0, 6\n\tmovgr2cf $fcc6, $t0\n\t"
        "srli.d $t0, %0, 7\n\tmovgr2cf $fcc7, $t0"
        : : "r"(value) : "$t0");
}

static uint64_t get_fcc(void)
{
    uint64_t value;
    uint64_t bit;
    __asm__ volatile(
        "movcf2gr %0, $fcc0\n\t"
        "movcf2gr %1, $fcc1\n\tslli.d %1, %1, 1\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc2\n\tslli.d %1, %1, 2\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc3\n\tslli.d %1, %1, 3\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc4\n\tslli.d %1, %1, 4\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc5\n\tslli.d %1, %1, 5\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc6\n\tslli.d %1, %1, 6\n\tor %0, %0, %1\n\t"
        "movcf2gr %1, $fcc7\n\tslli.d %1, %1, 7\n\tor %0, %0, %1"
        : "=&r"(value), "=&r"(bit));
    return value & 0xff;
}

static void signal_handler(int sig)
{
    static const uint64_t clobber[2] = {
        0xdeadbeefcafef00dULL, 0x0123456789abcdefULL
    };
    (void)sig;
    load_vr0(clobber);
    set_fcc(0x5a);
    handler_seen = 1;
}

static int signal_roundtrip(void)
{
    static const uint64_t expected[2] = {
        0x1122334455667788ULL, 0x99aabbccddeeff00ULL
    };
    uint64_t observed[2] = {0, 0};
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
        return 1;
    handler_seen = 0;
    load_vr0(expected);
    set_fcc(0xa5);
    if (kill(getpid(), SIGUSR1) < 0)
        return 1;
    store_vr0(observed);
    return !handler_seen || memcmp(expected, observed, sizeof(expected)) != 0 ||
           get_fcc() != 0xa5;
}

static int scheduling_roundtrip(unsigned seed)
{
    uint64_t expected[2] = {
        0x1000000000000000ULL | seed,
        0x8000000000000000ULL | ((uint64_t)seed << 32)
    };
    uint64_t observed[2] = {0, 0};
    load_vr0(expected);
    set_fcc(seed);
    for (int i = 0; i < 2000; i++)
        sched_yield();
    store_vr0(observed);
    return memcmp(expected, observed, sizeof(expected)) != 0 ||
           get_fcc() != (seed & 0xff);
}

int main(void)
{
    if (signal_roundtrip() != 0) {
        puts("ARCH_CONTEXT_STRESS: signal-lsx-fcc FAIL");
        return 1;
    }
    puts("ARCH_CONTEXT_STRESS: signal-lsx-fcc PASS");

    pid_t children[8];
    for (unsigned i = 0; i < 8; i++) {
        children[i] = fork();
        if (children[i] == 0)
            _exit(scheduling_roundtrip(0x81 + i));
        if (children[i] < 0)
            return 1;
    }
    for (unsigned i = 0; i < 8; i++) {
        int status = 0;
        if (waitpid(children[i], &status, 0) != children[i] ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            puts("ARCH_CONTEXT_STRESS: schedule-lsx-fcc FAIL");
            return 1;
        }
    }
    puts("ARCH_CONTEXT_STRESS: schedule-lsx-fcc PASS");
    puts("ARCH_CONTEXT_STRESS: PASS");
    return 0;
}

#else

int main(void)
{
    puts("ARCH_CONTEXT_STRESS: SKIP non-loongarch64");
    return 0;
}

#endif
