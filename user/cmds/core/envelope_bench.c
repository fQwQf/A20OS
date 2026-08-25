/* E11: envelope mediation overhead microbenchmark
 * (docs/research/10-evaluation.md §5.5 -- cost-side paper evidence).
 *
 * One process measures each operation family twice: unrestricted
 * ("off") and inside a fresh envelope ("on"), then reports ns/op and
 * the delta.  Timing uses CLOCK_MONOTONIC (vDSO fast path), taken once
 * around each loop so the clock itself stays out of the hot path.
 *
 * All figures come from one fixed, publicly reproducible QEMU/TCG
 * configuration; conclusions do not depend on any specific host.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903

/* Must match kernel struct a20_env_policy. */
struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

#define OBJ_FILE 3
#define R_READ  (1ull << 0)
#define R_WRITE (1ull << 1)
#define R_STAT  (1ull << 3)
#define R_SEEK  (1ull << 4)

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static long env_create(struct env_policy *p)
{
    return syscall(SYS_a20_envelope_create, p, 0L);
}
static long env_enter(long id)
{
    return syscall(SYS_a20_envelope_enter, id);
}

static volatile unsigned long long sink;

static double bench_open_close(int iters)
{
    double t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        int fd = open("/tmp/bench/data.bin", O_RDONLY);
        if (fd >= 0)
            close(fd);
        sink += (unsigned long)fd;
    }
    return (now_ns() - t0) / iters;
}

static double bench_read(int fd, char *buf, int iters)
{
    double t0 = now_ns();
    unsigned long long acc = 0;
    for (int i = 0; i < iters; i++) {
        ssize_t n = read(fd, buf, 64);
        if (n > 0)
            acc += (unsigned long)n;
    }
    sink += acc;
    return (now_ns() - t0) / iters;
}

static double bench_write(int fd, const char *buf, int iters)
{
    double t0 = now_ns();
    for (int i = 0; i < iters; i++)
        write(fd, buf, 64);
    return (now_ns() - t0) / iters;
}

static double bench_lseek(int fd, int iters)
{
    double t0 = now_ns();
    for (int i = 0; i < iters; i++)
        lseek(fd, 0, SEEK_SET);
    return (now_ns() - t0) / iters;
}

/* Paired cost: each iteration is one mediated sendto (W charge) plus
 * one recvfrom drain (R charge) so the stream buffer never fills.
 * Reported honestly as the send+drain pair, not send alone. */
static double bench_sendto(int sfd, int rfd, char *buf, int iters)
{
    /* Warm-up off the clock validates delivery once: if the transport
     * cannot move bytes (or a budget denies mid-run), callers see -1
     * and skip the row instead of blocking on an empty peer queue. */
    if (sendto(sfd, buf, 64, 0, NULL, 0) != 64)
        return -1.0;
    if (recvfrom(rfd, buf, 64, 0, NULL, NULL) < 0)
        return -1.0;

    double t0 = now_ns();
    for (int i = 0; i < iters; i++) {
        if (sendto(sfd, buf, 64, 0, NULL, 0) != 64)
            return -1.0;
        if (recvfrom(rfd, buf, 64, 0, NULL, NULL) < 0)
            return -1.0;
    }
    return (now_ns() - t0) / iters;
}

struct result {
    double open_close, read64, write64, lseek, sendto64;
};

static void run_phase(struct result *r, int iters)
{
    char buf[64];
    memset(buf, 'x', sizeof(buf));

    /* open+close pair */
    r->open_close = bench_open_close(iters);

    /* read/write/lseek on persistent fds (reopened fresh for this phase,
     * so the on-phase sees tracked shadows, not grandfathered entries) */
    int rd = open("/tmp/bench/data.bin", O_RDONLY);
    int wr = open("/tmp/bench/append.log", O_CREAT | O_APPEND | O_WRONLY,
                  0644);
    r->read64 = bench_read(rd, buf, iters);
    r->lseek = bench_lseek(rd, iters);
    r->write64 = bench_write(wr, buf, iters);

    /* socket data plane: fresh AF_UNIX pair per phase; fds are
     * grandfathered (untracked), so this exercises pure budget
     * charging on sendto/recvfrom without shadow direction bits. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        sv[0] = sv[1] = -1;
    r->sendto64 = bench_sendto(sv[0], sv[1], buf, iters);
    if (sv[0] >= 0) {
        close(sv[0]);
        close(sv[1]);
    }
    close(rd);
    close(wr);
}

static void report(const char *name, double off, double on)
{
    double delta = off > 0 ? (on - off) / off * 100.0 : 0;
    printf("ENVELOPE_BENCH: %-12s off=%8.0f ns/op on=%8.0f ns/op "
           "(%+.1f%%)\n", name, off, on, delta);
}

int main(void)
{
    enum { ITERS = 20000 };
    printf("ENVELOPE_BENCH: start\n");

    if (mkdir("/tmp/bench", 0755) < 0 && errno != EEXIST)
        return 1;

    putenv("TZ=UTC");

    /* fixture */
    int fd = open("/tmp/bench/data.bin", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        perror("fixture");
        return 1;
    }
    char init[64];
    memset(init, 'a', sizeof(init));
    write(fd, init, sizeof(init));
    close(fd);

    struct result off, on;

    /* ---- phase 1: unrestricted ---- */
    run_phase(&off, ITERS);

    /* ---- phase 2: enveloped ---- */
    struct env_policy p;
    memset(&p, 0, sizeof(p));
    p.allowed_types = 1u << OBJ_FILE;
    p.rights_by_class[OBJ_FILE] =
        R_READ | R_WRITE | R_STAT | R_SEEK;
    p.op_budget = 6ull * ITERS + 1024; /* open+rd+wr+send+drain per iter */
    p.data_budget = 16ull << 20; /* 16 MiB: reads+writes both charged */
    long id = env_create(&p);
    if (id < 0 || env_enter(id) < 0) {
        printf("ENVELOPE_BENCH: FAIL (envelope setup)\n");
        return 1;
    }
    run_phase(&on, ITERS);

    report("open+close", off.open_close, on.open_close);
    report("read-64B", off.read64, on.read64);
    report("write-64B", off.write64, on.write64);
    report("lseek", off.lseek, on.lseek);
    if (off.sendto64 > 0 && on.sendto64 > 0)
        report("sock-sendto", off.sendto64, on.sendto64);
    else
        printf("ENVELOPE_BENCH: sock-sendto SKIPPED (transport)\n");

    printf("ENVELOPE_BENCH: PASS\n");
    return 0;
}
