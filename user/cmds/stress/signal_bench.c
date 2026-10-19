/*
 * signal_bench — end-to-end signal path benchmark for C/R performance
 * pairing (Signal module goal).  Not a pass/fail stress test: it prints
 * measured latencies/throughputs that the C and Rust kernel selections are
 * compared against under an identical guest configuration.
 *
 * Three measurements, all end-to-end through the real kernel paths:
 *
 *  1. killwake:  this task loops kill(getpid(), SIGUSR1); the pending
 *     signal is delivered at the kill syscall-return boundary, so one
 *     iteration covers queue -> select -> delivery -> handler -> sigreturn.
 *  2. pingpong:  a receiver thread parks in sigsuspend(); the sender kills
 *     it per round, covering kill -> queue -> wake -> delivery -> handler
 *     -> return -> re-park.
 *  3. signalfd:  SIGUSR1/SIGUSR2 blocked, queued in batches via kill and
 *     drained with signalfd read(), covering queue -> readiness -> batch
 *     select/consume/record-format.
 *
 * Usage: signal_bench [rounds] [pingpong_rounds] [fd_records]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t killwake_count = 0;
static volatile sig_atomic_t pingpong_done = 0;
static volatile sig_atomic_t pingpong_reply = 0;
static volatile sig_atomic_t watchdog_phase = 0;

/* Stall diagnostics: print the pingpong counters periodically so a wedged
 * run shows which side stopped advancing. */
static void watchdog(int sig)
{
    (void)sig;
    printf("SIGNAL_BENCH: watchdog phase=%d done=%d reply=%d\n",
           (int)watchdog_phase, (int)pingpong_done, (int)pingpong_reply);
}

/* Per-round latency census for the pingpong phase. The kernel-side wake path
 * must show the same census for the C reference and the Rust core: a
 * tick-quantized tail (rounds near 10 ms or worse) points at a woken task
 * dispatched only by the target CPU's next periodic tick, not at user
 * variance. Counters are reset by the phase and are kept identical in both
 * tree's bench so the round-average comparison stays fair. */
static long ping_slow1ms, ping_slow10ms, ping_slow100ms;
static unsigned long long ping_max_ns;

static void killwake_handler(int sig) { (void)sig; killwake_count++; }

static pthread_t g_sender;
static void pingpong_handler(int sig)
{
    (void)sig;
    pingpong_done++;
    pthread_kill(g_sender, SIGUSR2);
}
static void reply_handler(int sig) { (void)sig; pingpong_reply++; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void install_watchdog(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watchdog;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_interval.tv_sec = 20;
    it.it_value.tv_sec = 20;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void install(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, NULL) != 0) {
        printf("SIGNAL_BENCH: FAIL sigaction sig=%d errno=%d\n", sig, errno);
        _exit(2);
    }
}

static int bench_killwake(long rounds, uint64_t *ns_out)
{
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    if (sigprocmask(SIG_UNBLOCK, &unblock, NULL) != 0)
        return -1;
    install(SIGUSR1, killwake_handler);

    killwake_count = 0;
    uint64_t t0 = now_ns();
    for (long i = 0; i < rounds; i++) {
        if (kill(getpid(), SIGUSR1) != 0)
            return -1;
        if (killwake_count != (sig_atomic_t)(i + 1)) {
            printf("SIGNAL_BENCH: FAIL killwake delivery lost at %ld (count=%d)\n",
                   i, (int)killwake_count);
            return -1;
        }
    }
    *ns_out = now_ns() - t0;
    return 0;
}

static void *pingpong_receiver(void *arg)
{
    (void)arg;
    sigset_t all, orig;
    sigfillset(&all);
    install(SIGUSR1, pingpong_handler);
    /* Save the original mask once; each sigsuspend(orig) waits with the
     * original (unblocked) set, because the kernel restores the blocking
     * mask in place of the temporary one after the wake. */
    if (sigprocmask(SIG_SETMASK, &all, &orig) != 0)
        return NULL;
    for (;;) {
        sigsuspend(&orig); /* wakes per round, handler replies */
    }
    return NULL;
}

static int bench_pingpong(long rounds, uint64_t *ns_out)
{
    pthread_t th;
    sigset_t none;
    install(SIGUSR2, reply_handler);
    g_sender = pthread_self();
    if (pthread_create(&th, NULL, pingpong_receiver, NULL) != 0)
        return -1;
    /* Let the receiver park in sigsuspend before the first kill. */
    usleep(20000);

    pingpong_done = 0;
    pingpong_reply = 0;
    ping_slow1ms = ping_slow10ms = ping_slow100ms = 0;
    ping_max_ns = 0;
    watchdog_phase = 1;
    install_watchdog();
    sigemptyset(&none);
    uint64_t t0 = now_ns();
    for (long i = 0; i < rounds; i++) {
        if (pthread_kill(th, SIGUSR1) != 0)
            return -1;
        /* Park until the receiver's handler replies; a reply that raced
         * ahead of the park is deliverable, so sigsuspend returns
         * immediately without losing it. */
        uint64_t r0 = now_ns();
        sigsuspend(&none);
        uint64_t dt = now_ns() - r0;
        ping_slow1ms += dt > 1000000ull;
        ping_slow10ms += dt > 10000000ull;
        ping_slow100ms += dt > 100000000ull;
        if (dt > ping_max_ns)
            ping_max_ns = dt;
    }
    uint64_t ns = now_ns() - t0;
    /* No pthread_cancel here: process exit reaps the receiver (musl
     * cancellation uses internal signal 33, whose cross-build behaviour is
     * out of scope for this harness). */
    if (pingpong_done != (sig_atomic_t)rounds ||
        pingpong_reply != (sig_atomic_t)rounds) {
        printf("SIGNAL_BENCH: FAIL pingpong delivered %d/%d replies %d/%ld\n",
               (int)pingpong_done, (int)rounds, (int)pingpong_reply, rounds);
        return -1;
    }
    *ns_out = ns;
    return 0;
}

/* The kernel queue is a per-signal bitmask (standard AND real-time signals
 * coalesce to one pending occurrence), so a batch cycles through distinct
 * signums to produce one record per kill. */
static int bench_signalfd(long records, uint64_t *ns_out, long *consumed_out)
{
    const int batch = 16; /* distinct signums SIGRTMIN .. SIGRTMIN+15 */
    sigset_t mask;
    sigemptyset(&mask);
    for (int i = 0; i < batch; i++)
        sigaddset(&mask, SIGRTMIN + i);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        return -1;
    int fd = signalfd(-1, &mask, 0);
    if (fd < 0)
        return -1;

    long consumed = 0;
    uint64_t t0 = now_ns();
    while (consumed < records) {
        int n = (int)((records - consumed) < batch ? (records - consumed) : batch);
        for (int i = 0; i < n; i++) {
            if (kill(getpid(), SIGRTMIN + i) != 0)
                return -1;
        }
        char buf[16 * 128];
        ssize_t got = read(fd, buf, sizeof(buf));
        if (got != n * 128) {
            printf("SIGNAL_BENCH: FAIL signalfd read got=%zd want=%d errno=%d\n",
                   got, n * 128, errno);
            return -1;
        }
        consumed += n;
    }
    *ns_out = now_ns() - t0;
    *consumed_out = consumed;
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? strtol(argv[1], NULL, 10) : 20000;
    long pp_rounds = argc > 2 ? strtol(argv[2], NULL, 10) : 5000;
    long fd_records = argc > 3 ? strtol(argv[3], NULL, 10) : 200000;
    if (rounds <= 0 || pp_rounds <= 0 || fd_records <= 0) {
        printf("SIGNAL_BENCH: FAIL bad argv\n");
        return 2;
    }

    uint64_t ns;
    if (bench_killwake(rounds, &ns) != 0) {
        printf("SIGNAL_BENCH: FAIL killwake errno=%d\n", errno);
        return 1;
    }
    printf("SIGNAL_BENCH: killwake rounds=%ld total_ns=%llu ns_per_op=%llu\n",
           rounds, (unsigned long long)ns, (unsigned long long)(ns / (uint64_t)rounds));

    if (bench_pingpong(pp_rounds, &ns) != 0) {
        printf("SIGNAL_BENCH: FAIL pingpong errno=%d\n", errno);
        return 1;
    }
    printf("SIGNAL_BENCH: pingpong rounds=%ld total_ns=%llu ns_per_round=%llu\n",
           pp_rounds, (unsigned long long)ns, (unsigned long long)(ns / (uint64_t)pp_rounds));
    printf("SIGNAL_BENCH: pingpong census slow1ms=%ld slow10ms=%ld slow100ms=%ld max_ns=%llu\n",
           ping_slow1ms, ping_slow10ms, ping_slow100ms, ping_max_ns);

    long consumed = 0;
    if (bench_signalfd(fd_records, &ns, &consumed) != 0) {
        printf("SIGNAL_BENCH: FAIL signalfd errno=%d\n", errno);
        return 1;
    }
    printf("SIGNAL_BENCH: signalfd records=%ld total_ns=%llu ns_per_record=%llu records_per_s=%llu\n",
           consumed, (unsigned long long)ns, (unsigned long long)(ns / (uint64_t)consumed),
           (unsigned long long)((uint64_t)consumed * 1000000000ull / ns));

    printf("SIGNAL_BENCH: DONE\n");
    return 0;
}
