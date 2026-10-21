/*
 * Bounded-progress probe: does an ordinary task keep making progress while
 * another task sits in a long kernel syscall and the CPU is taking an
 * interrupt storm?
 *
 * The contract this checks is the one kernel preemption exists for: a runnable
 * normal task must get the CPU within a bounded time even when the current
 * task will not return to userspace for a while.  A merely runnable task tells
 * nothing on its own -- the timer tick already reschedules at every syscall
 * return -- so two things are needed:
 *
 *   - a long kernel path with no internal reschedule point.  A single read()
 *     of a page-cache-warm file copies the whole count page by page
 *     (kernel/fs/page_cache.c); under TCG one read of 256 MiB takes hundreds of
 *     milliseconds, which is exactly one long syscall.
 *   - steady interrupt load, so the IRQ-return preemption decision point runs
 *     constantly instead of once per tick.  A POSIX interval timer at
 *     STORM_INTERVAL_US fires a real timer interrupt per expiry; the child that
 *     owns it counts expiries so the run can prove the storm happened.
 *
 * The probe child runs at ordinary priority, sleeps a fixed interval and
 * reports how late each wakeup ran.  With bounded progress the lateness is a
 * tick or two; if a long syscall were allowed to run to completion, every
 * sample would be bounded only by the remaining time in the hog's current
 * read().  The whole distribution is printed, not just the maximum.
 *
 * Usage: bounded_progress [iterations] [cpu]
 *
 *   cpu >= 0 pins the hog, the storm and the probe to that CPU.  On an SMP
 *   guest an unpinned probe would simply run on the idle CPU and the number
 *   would say nothing about preemption.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FILE_MB            256
#define HOG_MS             5000
#define DEFAULT_ITERATIONS 20
#define SLEEP_NS           20000000L    /* 20ms between probe samples */
#define STORM_INTERVAL_US  100          /* 10k timer interrupts/second */

/* Maximum tolerated wakeup lateness for an ordinary task.  Measured runs on
 * TCG put the maximum at a few tens of milliseconds; this bound is an order of
 * magnitude above that, so it fails on "the hog ran to completion" (hundreds of
 * milliseconds) and not on scheduler noise. */
#define LATE_BOUND_NS      150000000ULL

static volatile sig_atomic_t storm_expiries;

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int pin_to_cpu(int cpu)
{
    if (cpu < 0)
        return -2;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static const char *const tmp_candidates[] = { "/tmp", "/var/tmp", "/", NULL };

static int make_backing_file(size_t bytes, char *path, size_t pathsz)
{
    size_t chunk = 1u << 20;
    char *zero = mmap(NULL, chunk, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (zero == MAP_FAILED)
        return -1;
    memset(zero, 0x5a, chunk);
    for (int i = 0; tmp_candidates[i]; i++) {
        snprintf(path, pathsz, "%s/bounded_progress.dat", tmp_candidates[i]);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            continue;
        /* Written for real, not ftruncate()d: a hole reads back without
         * entering the page-cache copy path this probe depends on. */
        int ok = 1;
        for (size_t done = 0; ok && done < bytes; ) {
            size_t n = bytes - done < chunk ? bytes - done : chunk;
            if (write(fd, zero, n) != (ssize_t)n)
                ok = 0;
            done += n;
        }
        close(fd);
        if (ok) {
            munmap(zero, chunk);
            return 0;
        }
        unlink(path);
    }
    munmap(zero, chunk);
    return -1;
}

struct hog_report {
    uint64_t reads;
    uint64_t max_read_ns;
    int err;
};

static void hog_main(const char *path, int report_fd, int cpu)
{
    struct hog_report r = { 0, 0, 0 };
    size_t read_bytes = (size_t)FILE_MB << 20;
    char *buf = mmap(NULL, read_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        r.err = errno;
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }
    pin_to_cpu(cpu);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        r.err = errno;
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }
    /* Warm every page first, so the timed loop measures the page-cache copy
     * and not virtio block traffic -- a disk read would leave the CPU idle. */
    if (read(fd, buf, read_bytes) != (ssize_t)read_bytes) {
        r.err = errno ? errno : -EIO;
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }
    uint64_t begin = now_ns();
    while (now_ns() - begin < (uint64_t)HOG_MS * 1000000ULL) {
        if (lseek(fd, 0, SEEK_SET) < 0)
            break;
        uint64_t t0 = now_ns();
        ssize_t n = read(fd, buf, read_bytes);
        uint64_t dt = now_ns() - t0;
        if (n != (ssize_t)read_bytes)
            break;
        if (dt > r.max_read_ns)
            r.max_read_ns = dt;
        r.reads++;
    }
    close(fd);
    write(report_fd, &r, sizeof(r));
    _exit(0);
}

static void storm_handler(int sig)
{
    (void)sig;
    storm_expiries++;
}

/* Keep the timer firing for the whole run and report how many expiries it
 * actually delivered; without that count a "no storm" run would look identical
 * to a passing one. */
static void storm_main(int report_fd, int cpu)
{
    pin_to_cpu(cpu);
    struct sigaction act = { .sa_handler = storm_handler };
    sigemptyset(&act.sa_mask);
    sigaction(SIGALRM, &act, NULL);

    struct itimerval it = {
        .it_interval = { .tv_sec = 0, .tv_usec = STORM_INTERVAL_US },
        .it_value = { .tv_sec = 0, .tv_usec = STORM_INTERVAL_US },
    };
    if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
        int rc = -errno;
        write(report_fd, &rc, sizeof(rc));
        _exit(1);
    }
    /* Sleep in short slices so the handler actually runs; a single long sleep
     * would still deliver signals, but slicing keeps the run responsive. */
    uint64_t begin = now_ns();
    while (now_ns() - begin < ((uint64_t)HOG_MS + 2000) * 1000000ULL)
        usleep(1000);
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    int count = (int)storm_expiries;
    write(report_fd, &count, sizeof(count));
    _exit(0);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int probe_main(int iterations, int result_fd, int cpu)
{
    pin_to_cpu(cpu);
    uint64_t *late = calloc((size_t)iterations, sizeof(*late));
    if (!late)
        return 100;
    int completed = 0;
    for (int i = 0; i < iterations; i++) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = SLEEP_NS };
        uint64_t t0 = now_ns();
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
        uint64_t t1 = now_ns();
        late[i] = (t1 - t0) > (uint64_t)SLEEP_NS
                      ? (t1 - t0) - (uint64_t)SLEEP_NS
                      : 0;
        completed++;
    }
    qsort(late, (size_t)completed, sizeof(*late), cmp_u64);
    int rc = 0;
    if (write(result_fd, &completed, sizeof(completed)) !=
            (ssize_t)sizeof(completed) ||
        write(result_fd, late, (size_t)completed * sizeof(*late)) !=
            (ssize_t)((size_t)completed * sizeof(*late)))
        rc = 101;
    free(late);
    return rc;
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : DEFAULT_ITERATIONS;
    int cpu = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
    if (iterations < 1 || iterations > 200) {
        printf("BOUNDED_PROGRESS: FAIL bad iterations=%d\n", iterations);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("BOUNDED_PROGRESS: start iterations=%d cpu=%d file_mb=%d "
           "hog_ms=%d storm_us=%d late_bound_ms=%llu\n", iterations, cpu,
           FILE_MB, HOG_MS, STORM_INTERVAL_US,
           (unsigned long long)(LATE_BOUND_NS / 1000000ULL));

    char path[256];
    if (make_backing_file((size_t)FILE_MB << 20, path, sizeof(path)) < 0) {
        printf("BOUNDED_PROGRESS: FAIL backing-file errno=%d\n", errno);
        return 1;
    }

    int hog_pipe[2], storm_pipe[2], probe_pipe[2];
    if (pipe(hog_pipe) || pipe(storm_pipe) || pipe(probe_pipe)) {
        printf("BOUNDED_PROGRESS: FAIL pipe errno=%d\n", errno);
        return 1;
    }

    pid_t hog = fork();
    if (hog == 0) {
        close(hog_pipe[0]);
        hog_main(path, hog_pipe[1], cpu);
    }
    pid_t storm = fork();
    if (storm == 0) {
        close(storm_pipe[0]);
        storm_main(storm_pipe[1], cpu);
    }
    /* Start the probe only after the hog has had time to reach its read loop,
     * so the samples land inside the long syscall rather than beside it. */
    usleep(300000);
    pid_t probe = fork();
    if (probe == 0) {
        close(probe_pipe[0]);
        _exit(probe_main(iterations, probe_pipe[1], cpu));
    }
    if (hog < 0 || storm < 0 || probe < 0) {
        printf("BOUNDED_PROGRESS: FAIL fork errno=%d\n", errno);
        return 1;
    }
    close(hog_pipe[1]);
    close(storm_pipe[1]);
    close(probe_pipe[1]);

    struct hog_report hr = { 0, 0, -1 };
    int storm_count = -1;
    int completed = 0;
    uint64_t *late = calloc((size_t)iterations, sizeof(*late));
    int failures = 0;

    if (!late)
        return 1;
    if (read(hog_pipe[0], &hr, sizeof(hr)) != (ssize_t)sizeof(hr)) {
        printf("BOUNDED_PROGRESS: FAIL hog-report short\n");
        failures++;
    }
    if (read(storm_pipe[0], &storm_count, sizeof(storm_count)) !=
        (ssize_t)sizeof(storm_count)) {
        printf("BOUNDED_PROGRESS: FAIL storm-report short\n");
        failures++;
    }
    ssize_t got = read(probe_pipe[0], &completed, sizeof(completed));
    if (got == (ssize_t)sizeof(completed))
        got = read(probe_pipe[0], late, (size_t)completed * sizeof(*late));
    if (got != (ssize_t)((size_t)completed * sizeof(*late))) {
        printf("BOUNDED_PROGRESS: FAIL probe-report short\n");
        failures++;
    }

    int status = 0;
    for (pid_t p = 0; p < 3; p++) {
        pid_t child = p == 0 ? hog : p == 1 ? storm : probe;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            printf("BOUNDED_PROGRESS: FAIL child=%d status=%d\n", (int)p,
                   status);
            failures++;
        }
    }
    unlink(path);

    if (hr.err != 0 || hr.reads == 0) {
        printf("BOUNDED_PROGRESS: FAIL hog err=%d reads=%llu\n", hr.err,
               (unsigned long long)hr.reads);
        failures++;
    } else {
        printf("BOUNDED_PROGRESS: hog reads=%llu max_read_ms=%llu PASS\n",
               (unsigned long long)hr.reads,
               (unsigned long long)(hr.max_read_ns / 1000000ULL));
    }
    if (storm_count <= 0) {
        printf("BOUNDED_PROGRESS: FAIL storm expiries=%d\n", storm_count);
        failures++;
    } else {
        printf("BOUNDED_PROGRESS: storm expiries=%d PASS\n", storm_count);
    }

    uint64_t max_late = 0, sum_late = 0;
    for (int i = 0; i < completed; i++) {
        if (late[i] > max_late)
            max_late = late[i];
        sum_late += late[i];
    }
    printf("BOUNDED_PROGRESS: probe samples_ms=");
    for (int i = 0; i < completed; i++)
        printf("%s%llu", i ? "," : "", (unsigned long long)(late[i] / 1000000ULL));
    printf("\n");
    if (completed != iterations) {
        printf("BOUNDED_PROGRESS: FAIL probe completed=%d want=%d\n",
               completed, iterations);
        failures++;
    }
    if (max_late > LATE_BOUND_NS) {
        printf("BOUNDED_PROGRESS: FAIL max_late_ms=%llu bound_ms=%llu\n",
               (unsigned long long)(max_late / 1000000ULL),
               (unsigned long long)(LATE_BOUND_NS / 1000000ULL));
        failures++;
    } else {
        printf("BOUNDED_PROGRESS: probe max_late_ms=%llu mean_late_ms=%llu "
               "PASS\n", (unsigned long long)(max_late / 1000000ULL),
               (unsigned long long)(completed ? (sum_late / (uint64_t)completed) / 1000000ULL : 0));
    }

    free(late);
    if (failures)
        return 1;
    printf("BOUNDED_PROGRESS: PASS\n");
    return 0;
}
