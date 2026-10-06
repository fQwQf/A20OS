/*
 * Kernel-preemption latency probe.
 *
 * The question this answers is narrow: while a task is inside a long kernel
 * syscall, can a higher-priority runnable task take the CPU before that
 * syscall returns?  A task that is merely runnable tells us nothing -- the
 * ordinary timer tick already reschedules at every syscall return -- so the
 * hog must not come back to userspace between chunks of work.
 *
 * page_cache_read_vfile() copies the whole count in one call, a page at a
 * time, with no reschedule point between pages (kernel/fs/page_cache.c), so a
 * single read() of a page-cache-warm file is exactly the "one long syscall"
 * this needs.  /dev/zero would not do: its read is one compiler-optimised
 * memset (kernel/fs/devfs/devfs_mem.c) and retires an order of magnitude
 * sooner than the per-page page-cache loop.
 *
 * Layout: a hog child keeps re-reading the file for HOG_MS; an RT child wakes
 * from nanosleep() ITER times under SCHED_FIFO and reports how late it was.
 * The gap between the two numbers is the verdict -- if the kernel can cut a
 * syscall short the RT wakeups land within a tick or two of their deadline,
 * and if it cannot they land wherever the hog's current read() happens to end.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HOG_MS        5000
#define RT_ITERATIONS 20
#define RT_SLEEP_NS   20000000L   /* 20ms: long enough to sleep, short
                                   * enough that 20 samples still fit inside
                                   * the hog's run. */
#define RT_PRIO       10

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t ms_of(uint64_t ns)
{
    return ns / 1000000ULL;
}

/* The hog's own view of its syscall cost travels back over this pipe so the
 * parent can print it next to the RT number; a shell pipeline would have to
 * interleave two children's output. */
struct hog_report {
    uint64_t reads;
    uint64_t max_read_ns;
    uint64_t bytes_per_read;
    int      err;
    int      pin_rc;
};

static const char *const tmp_candidates[] = {
    "/tmp", "/var/tmp", "/", "/bin", NULL
};

static int make_backing_file(size_t bytes)
{
    size_t chunk = 1u << 20;
    char *zero = mmap(NULL, chunk, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (zero == MAP_FAILED)
        return -1;

    for (int i = 0; tmp_candidates[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/preempt_lat.dat", tmp_candidates[i]);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            continue;
        /* Written for real rather than ftruncate()d: a hole reads back without
         * ever entering the disk path, and whether this ext4 keeps the hole
         * sparse is not something this probe should depend on. */
        int ok = 1;
        for (size_t done = 0; ok && done < bytes; ) {
            size_t n = bytes - done < chunk ? bytes - done : chunk;
            ssize_t w = write(fd, zero, n);
            if (w != (ssize_t)n)
                ok = 0;
            done += n;
        }
        close(fd);
        if (ok)
            return 0;
        unlink(path);
    }
    munmap(zero, chunk);
    return -1;
}

/* Pin the calling task to one CPU, or skip silently for cpu < 0.  On an SMP
 * guest the unpinned RT child would simply be placed on the idle CPU and the
 * number would say nothing about preemption -- same-CPU contention has to be
 * forced for the measurement to mean what it means on a single-CPU guest. */
static int pin_to_cpu(int cpu)
{
    if (cpu < 0)
        return -2;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static int hog_main(const char *path, size_t read_bytes, int report_fd,
                    int pin_cpu)
{
    int pin_rc = pin_to_cpu(pin_cpu);
    char *buf = mmap(NULL, read_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        struct hog_report r = { 0, 0, 0, errno, pin_rc };
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        struct hog_report r = { 0, 0, 0, errno, pin_rc };
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }

    /* First pass pulls every page in, so the timed loop measures the page
     * cache copy rather than virtio block traffic -- a disk read would leave
     * the CPU idle and both children would look on time. */
    ssize_t warm = read(fd, buf, read_bytes);
    if (warm != (ssize_t)read_bytes) {
        struct hog_report r = { 0, 0, (uint64_t)read_bytes,
                                warm < 0 ? errno : -EIO, pin_rc };
        write(report_fd, &r, sizeof(r));
        _exit(1);
    }

    uint64_t begin = now_ns();
    uint64_t max_read_ns = 0;
    uint64_t reads = 0;
    while (now_ns() - begin < (uint64_t)HOG_MS * 1000000ULL) {
        if (lseek(fd, 0, SEEK_SET) < 0)
            break;
        uint64_t t0 = now_ns();
        ssize_t n = read(fd, buf, read_bytes);
        uint64_t dt = now_ns() - t0;
        if (n != (ssize_t)read_bytes)
            break;
        if (dt > max_read_ns)
            max_read_ns = dt;
        reads++;
    }

    struct hog_report r = { reads, max_read_ns, (uint64_t)read_bytes, 0,
                             pin_rc };
    write(report_fd, &r, sizeof(r));
    close(fd);
    _exit(0);
}

static int rt_main(int result_fd, int pin_cpu)
{
    int pin_rc = pin_to_cpu(pin_cpu);
    struct sched_param param = { .sched_priority = RT_PRIO };
    int prio_rc = sched_setscheduler(0, SCHED_FIFO, &param);
    int policy = sched_getscheduler(0);

    /* Every sample is reported, not just the maximum: a single blown deadline
     * among twenty tells a different story from twenty blown deadlines, and
     * only the distribution says which one this run saw. */
    uint64_t late[RT_ITERATIONS];
    uint64_t max_wakeup_ns = 0;
    uint64_t sum_wakeup_ns = 0;
    for (int i = 0; i < RT_ITERATIONS; i++) {
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = RT_SLEEP_NS,
        };
        uint64_t t0 = now_ns();
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
        uint64_t t1 = now_ns();
        uint64_t l = (t1 - t0) > (uint64_t)RT_SLEEP_NS
                         ? (t1 - t0) - (uint64_t)RT_SLEEP_NS
                         : 0;
        late[i] = l;
        if (l > max_wakeup_ns)
            max_wakeup_ns = l;
        sum_wakeup_ns += l;
    }

    /* prio_rc/policy/pin_rc ride along so the parent can say whether the
     * FIFO request and the affinity request were honoured instead of
     * silently testing normal priority on the wrong CPU. */
    uint64_t tail[4] = { max_wakeup_ns, sum_wakeup_ns,
                         (uint64_t)(((int64_t)prio_rc << 32) | (policy & 0xffffffff)),
                         (uint64_t)(int64_t)pin_rc };
    if (write(result_fd, late, sizeof(late)) != (ssize_t)sizeof(late))
        _exit(1);
    write(result_fd, tail, sizeof(tail));
    _exit(0);
}

int main(int argc, char **argv)
{
    /* Defaults sized for a 1-2G guest: a 32MB file plus a 32MB read buffer.
     * Shrink both on a small instance -- the interesting property is that one
     * read() outlasts RT_ITERATIONS * RT_SLEEP_NS several times over. */
    size_t mb = argc > 1 ? (size_t)strtoul(argv[1], NULL, 0) : 32;
    if (mb == 0 || mb > 512) {
        printf("PREEMPT_LAT: FAIL bad size_mb=%zu\n", mb);
        return 1;
    }
    size_t read_bytes = mb << 20;
    /* Optional CPU pinning for SMP guests: hog and RT pinned to the same CPU
     * reconstruct the single-CPU contention there; negative values leave the
     * tasks to the scheduler. */
    int hog_cpu = argc > 2 ? (int)strtol(argv[2], NULL, 0) : -1;
    int rt_cpu = argc > 3 ? (int)strtol(argv[3], NULL, 0) : -1;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("PREEMPT_LAT: start size_mb=%zu rt_iterations=%d rt_sleep_ms=%d "
           "hog_ms=%d hog_cpu=%d rt_cpu=%d\n", mb, RT_ITERATIONS,
           (int)(RT_SLEEP_NS / 1000000L), HOG_MS, hog_cpu, rt_cpu);

    if (make_backing_file(read_bytes) < 0) {
        printf("PREEMPT_LAT: FAIL cannot create backing file errno=%d\n",
               errno);
        return 1;
    }
    /* Every candidate is tried in make_backing_file(); re-derive which one
     * survived so the log names the file the hog actually read. */
    const char *path = NULL;
    char pathbuf[256];
    for (int i = 0; tmp_candidates[i] && !path; i++) {
        snprintf(pathbuf, sizeof(pathbuf), "%s/preempt_lat.dat",
                 tmp_candidates[i]);
        struct stat st;
        if (stat(pathbuf, &st) == 0 && (size_t)st.st_size == read_bytes)
            path = pathbuf;
    }
    if (!path) {
        printf("PREEMPT_LAT: FAIL backing file not found\n");
        return 1;
    }
    printf("PREEMPT_LAT: backing=%s bytes=%zu\n", path, read_bytes);

    int hog_pipe[2], rt_pipe[2];
    if (pipe(hog_pipe) < 0 || pipe(rt_pipe) < 0) {
        printf("PREEMPT_LAT: FAIL pipe errno=%d\n", errno);
        return 1;
    }

    pid_t hog = fork();
    if (hog == 0) {
        close(hog_pipe[0]);
        close(rt_pipe[0]);
        close(rt_pipe[1]);
        hog_main(path, read_bytes, hog_pipe[1], hog_cpu);
        _exit(9);
    }
    pid_t rt = fork();
    if (rt == 0) {
        close(rt_pipe[0]);
        close(hog_pipe[0]);
        close(hog_pipe[1]);
        rt_main(rt_pipe[1], rt_cpu);
        _exit(9);
    }
    close(hog_pipe[1]);
    close(rt_pipe[1]);

    if (hog < 0 || rt < 0) {
        printf("PREEMPT_LAT: FAIL fork errno=%d\n", errno);
        return 1;
    }

    struct hog_report rep;
    memset(&rep, 0, sizeof(rep));
    if (read(hog_pipe[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        rep.err = errno ? errno : EIO;
    close(hog_pipe[0]);

    uint64_t late[RT_ITERATIONS] = { 0 };
    uint64_t out[4] = { 0, 0, 0, 0 };
    if (read(rt_pipe[0], late, sizeof(late)) != (ssize_t)sizeof(late))
        memset(late, 0, sizeof(late));
    read(rt_pipe[0], out, sizeof(out));
    close(rt_pipe[0]);

    int hs = 0, rs = 0;
    waitpid(hog, &hs, 0);
    waitpid(rt, &rs, 0);

    int prio_rc = (int)(int32_t)(uint32_t)(out[2] >> 32);
    int policy = (int)(uint32_t)out[2];
    int rt_pin_rc = (int)(int64_t)out[3];

    printf("PREEMPT_LAT: hog reads=%llu max_read_ms=%llu bytes=%llu err=%d "
           "pin_rc=%d\n",
           (unsigned long long)rep.reads, (unsigned long long)ms_of(rep.max_read_ns),
           (unsigned long long)rep.bytes_per_read, rep.err, rep.pin_rc);
    printf("PREEMPT_LAT: rt sched_setscheduler_rc=%d policy=%d pin_rc=%d "
           "max_wakeup_ms=%llu avg_wakeup_ms=%llu\n",
           prio_rc, policy, rt_pin_rc, (unsigned long long)ms_of(out[0]),
           (unsigned long long)ms_of(RT_ITERATIONS ? out[1] / RT_ITERATIONS : 0));
    printf("PREEMPT_LAT: rt_samples_ms=");
    for (int i = 0; i < RT_ITERATIONS; i++)
        printf("%s%llu", i ? "," : "", (unsigned long long)ms_of(late[i]));
    printf("\n");

    if (rep.err || !WIFEXITED(hs) || !WIFEXITED(rs) || out[0] == 0) {
        printf("PREEMPT_LAT: FAIL incomplete measurement\n");
        return 1;
    }
    printf("PREEMPT_LAT: DONE\n");
    return 0;
}