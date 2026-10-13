#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <uapi/a20/block.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Same Linux/POSIX program on an ordinary SMP kernel or a shared-kernel
 * cluster. No cluster syscall, host state injection or special libc. */
#define ROUNDS 64
#define JOBS 1024
static pthread_barrier_t gate;
static pthread_key_t key;
static uint32_t *page;
static unsigned next_job, cpu_mask, destructed, migrations, faults, visits[JOBS];
static uint64_t sums[2];
static unsigned jobs[2];
static __thread volatile uint64_t tls_word;
static __thread sigjmp_buf fault_return;
static __thread volatile sig_atomic_t expecting_fault;
static void protection_fault(int signum, siginfo_t *info, void *context) {
    (void)context;
    if (signum != SIGSEGV || !expecting_fault || info->si_addr != &page[2])
        _Exit(2);
    __atomic_fetch_add(&faults, 1, __ATOMIC_RELAXED);
    siglongjmp(fault_return, 1);
}

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "CLUSTER_THREADS: FAIL %s errno=%d\n", what, errno);
        _Exit(1);
    }
}
static void barrier(void) {
    int r = pthread_barrier_wait(&gate);
    check(!r || r == PTHREAD_BARRIER_SERIAL_THREAD, "barrier");
}
static void bind(unsigned mask) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (unsigned i = 0; i < 2; i++)
        if (mask & (1U << i)) CPU_SET(i, &cpus);
    check(!sched_setaffinity(0, sizeof(cpus), &cpus), "affinity");
}
static void destructor(void *p) {
    check((uintptr_t)p == 1 || (uintptr_t)p == 2, "pthread key value");
    __atomic_fetch_add(&destructed, 1, __ATOMIC_RELAXED);
}
static void *worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    volatile uint64_t stack_word = 0xa20c0000ULL + id;
    tls_word = 0x123456780000ULL + id;
    check(!pthread_setspecific(key, (void *)(uintptr_t)(id + 1)), "setspecific");
    for (unsigned r = 0; r < ROUNDS; r++) {
        unsigned cpu = (r + id) & 1;
        int before = sched_getcpu();
        bind(1U << cpu);
        if (before != (int)cpu) __atomic_fetch_add(&migrations, 1, __ATOMIC_RELAXED);
        check(sched_getcpu() == (int)cpu, "migration CPU");
        check(tls_word == 0x123456780000ULL + id + r, "TLS across migration");
        check(stack_word == 0xa20c0000ULL + id, "stack across migration");
        check((uintptr_t)pthread_getspecific(key) == id + 1, "pthread key across migration");
        tls_word++;
        barrier(); /* Main publishes a read-only page. */
        check(page[0] == r + 1, "shared read-only mapping");
        if (!sigsetjmp(fault_return, 1)) {
            expecting_fault = 1;
            *(volatile uint32_t *)&page[2] = id + 1;
            check(0, "remote read-only write did not fault");
        }
        expecting_fault = 0;
        barrier();
        barrier(); /* Main restores write permission. */
        __atomic_fetch_add(&page[1], 1, __ATOMIC_SEQ_CST);
        barrier();
    }
    bind(3);
    barrier();
    for (;;) {
        unsigned job = __atomic_fetch_add(&next_job, 1, __ATOMIC_RELAXED);
        if (job >= JOBS) break;
        check(!__atomic_fetch_add(&visits[job], 1, __ATOMIC_RELAXED), "duplicate job");
        unsigned cpu = (unsigned)sched_getcpu();
        check(cpu < 2, "work CPU");
        __atomic_fetch_or(&cpu_mask, 1U << cpu, __ATOMIC_RELAXED);
        uint64_t sum = 0;
        for (unsigned k = 1; k <= 128; k++) sum += (uint64_t)k * (job + 1);
        sums[id] += sum;
        jobs[id]++;
        sched_yield();
    }
    return (void *)(uintptr_t)(id + 17);
}
static void disk_roundtrip(void) {
    bind(2);
    check(sched_getcpu() == 1, "remote disk CPU");
    int fd = open("/dev/disk0", O_RDWR);
    check(fd >= 0, "open scratch disk");
    uint64_t capacity = 0;
    uint32_t sector_size = 0;
    check(!ioctl(fd, A20_BLK_IOCTL_GET_CAPACITY, &capacity) && capacity == 4096 &&
          !ioctl(fd, A20_BLK_IOCTL_GET_SECTOR_SZ, &sector_size) && sector_size == 512,
          "scratch disk geometry");
    unsigned char buffer[512];
    check(lseek(fd, 7 * 512, SEEK_SET) == 7 * 512 && read(fd, buffer, 512) == 512,
          "read scratch signature");
    for (unsigned i = 0; i < 512; i++)
        check(buffer[i] == (unsigned char)(i * 17 + 0xa2), "scratch signature");
    for (unsigned round = 0; round < 32; round++) {
        off_t offset = (8 + round) * 512;
        for (unsigned i = 0; i < 512; i++) buffer[i] = (unsigned char)(i * 29 + round * 7 + 0x20);
        check(lseek(fd, offset, SEEK_SET) == offset && write(fd, buffer, 512) == 512,
              "remote disk write");
        /* Existing raw block ioctl: fsync on a raw devfs node does not flush
         * the hardware. A mounted filesystem uses its normal fsync path. */
        check(!ioctl(fd, A20_BLK_IOCTL_SYNC), "remote disk flush");
        memset(buffer, 0, sizeof(buffer));
        check(lseek(fd, offset, SEEK_SET) == offset && read(fd, buffer, 512) == 512,
              "remote disk readback");
        for (unsigned i = 0; i < 512; i++)
            check(buffer[i] == (unsigned char)(i * 29 + round * 7 + 0x20), "disk data");
        check(sched_getcpu() == 1, "disk affinity preserved");
    }
    check(!close(fd), "close scratch disk");
    puts("CLUSTER_DISK: PASS cpu=1 sectors=32 bytes=16384 flush=32 readback=32");
}

static int filesystem_child(void) {
    check(sched_getcpu() == 1, "exec preserved remote CPU affinity");
    int fd = open("/mnt/result.tmp", O_CREAT | O_EXCL | O_RDWR, 0600);
    check(fd >= 0, "create filesystem output");
    unsigned char buffer[4096];
    for (unsigned block = 0; block < 16; block++) {
        for (unsigned i = 0; i < sizeof(buffer); i++)
            buffer[i] = (unsigned char)(i * 13 + block * 7 + 0xa2);
        check(write(fd, buffer, sizeof(buffer)) == sizeof(buffer), "filesystem write");
    }
    check(!fsync(fd) && !close(fd), "filesystem fsync/close");
    check(!rename("/mnt/result.tmp", "/mnt/result.bin"), "filesystem rename");
    fd = open("/mnt", O_RDONLY | O_DIRECTORY);
    check(fd >= 0 && !fsync(fd) && !close(fd), "filesystem directory sync");
    check(sched_getcpu() == 1, "filesystem remote execution");
    puts("CLUSTER_FS_CHILD: PASS cpu=1 bytes=65536 fsync=file+directory");
    return 37;
}
static void filesystem_roundtrip(void) {
    bind(2);
    check(!mkdir("/mnt", 0755) || errno == EEXIST, "mount directory");
    check(!mount("/dev/vda", "/mnt", "ext4", 0, NULL), "mount scratch ext4");
    pid_t pid = fork();
    check(pid >= 0, "filesystem fork");
    if (!pid) {
        execl("/mnt/program", "program", "--filesystem-child", (char *)NULL);
        check(0, "exec from disk");
    }
    int status = 0;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 37,
          "filesystem child exit");
    int fd = open("/mnt/result.bin", O_RDONLY);
    check(fd >= 0, "reopen filesystem output");
    unsigned char buffer[4096];
    for (unsigned block = 0; block < 16; block++) {
        check(read(fd, buffer, sizeof(buffer)) == sizeof(buffer), "filesystem readback");
        for (unsigned i = 0; i < sizeof(buffer); i++)
            check(buffer[i] == (unsigned char)(i * 13 + block * 7 + 0xa2), "filesystem data");
    }
    check(read(fd, buffer, 1) == 0 && !close(fd), "filesystem EOF/close");
    check(!umount("/mnt"), "filesystem unmount");
    check(sched_getcpu() == 1, "filesystem affinity preserved");
    puts("CLUSTER_FS: PASS cpu=1 exec=disk child-exit=37 bytes=65536 unmounted=1");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--filesystem-child")) return filesystem_child();
    int console = 0, disk = 0, filesystem = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--console")) console = 1;
        else if (!strcmp(argv[i], "--disk")) disk = 1;
        else if (!strcmp(argv[i], "--filesystem")) filesystem = 1;
        else check(0, "arguments");
    }
    struct sigaction action = {.sa_sigaction = protection_fault, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    check(!sigaction(SIGSEGV, &action, NULL), "sigaction");
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(page != MAP_FAILED, "mmap");
    check(!pthread_barrier_init(&gate, NULL, 3), "barrier init");
    check(!pthread_key_create(&key, destructor), "key create");
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++)
        check(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i), "pthread_create");
    for (unsigned r = 0; r < ROUNDS; r++) {
        page[0] = r + 1;
        check(!mprotect(page, 4096, PROT_READ), "mprotect read-only");
        barrier();
        barrier();
        check(!mprotect(page, 4096, PROT_READ | PROT_WRITE), "mprotect writable");
        barrier();
        barrier();
        check(page[1] == 2 * (r + 1), "remote writes after TLB shootdown");
    }
    barrier();
    for (unsigned i = 0; i < 2; i++) {
        void *result = NULL;
        check(!pthread_join(threads[i], &result) && (uintptr_t)result == i + 17, "join");
    }
    check(destructed == 2 && jobs[0] && jobs[1] && jobs[0] + jobs[1] == JOBS && cpu_mask == 3 &&
          migrations >= 126 && migrations <= 128 && faults == 2 * ROUNDS, "work distribution");
    uint64_t expected = 8256ULL * JOBS * (JOBS + 1) / 2;
    check(sums[0] + sums[1] == expected, "work sum");
    for (unsigned i = 0; i < JOBS; i++) check(visits[i] == 1, "missing job");
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_condattr_t attr;
    pthread_cond_t cond;
    check(!pthread_condattr_init(&attr) && !pthread_condattr_setclock(&attr, CLOCK_MONOTONIC), "cond clock");
    check(!pthread_cond_init(&cond, &attr) && !pthread_mutex_lock(&mutex), "cond init");
    struct timespec until, now;
    check(!clock_gettime(CLOCK_MONOTONIC, &until), "clock");
    until.tv_nsec += 20000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    check(pthread_cond_timedwait(&cond, &mutex, &until) == ETIMEDOUT, "timed cond wait");
    check(!clock_gettime(CLOCK_MONOTONIC, &now) && (now.tv_sec > until.tv_sec ||
          (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)), "timeout clock");
    check(!pthread_mutex_unlock(&mutex) && !pthread_cond_destroy(&cond) &&
          !pthread_condattr_destroy(&attr) && !pthread_mutex_destroy(&mutex), "sync cleanup");
    check(!pthread_key_delete(key) && !pthread_barrier_destroy(&gate) && !munmap(page, 4096), "cleanup");
    if (console) {
        bind(2);
        check(sched_getcpu() == 1, "remote console CPU");
        puts("CLUSTER_CONSOLE: remote-cpu=1 ready");
        char input[32];
        check(fgets(input, sizeof(input), stdin) && !strcmp(input, "owner-ok\n"),
              "remote console input");
        for (unsigned i = 0; i < 32; i++)
            printf("CLUSTER_CONSOLE_BLOCK:%02u:abcdefghijklmnopqrstuvwxyz012345\n", i);
        check(sched_getcpu() == 1, "console affinity preserved");
        puts("CLUSTER_CONSOLE: PASS cpu=1 input=owner-ok blocks=32");
    }
    if (disk) disk_roundtrip();
    if (filesystem) filesystem_roundtrip();
    printf("CLUSTER_THREADS: PASS threads=2 affinity-rounds=128 migrations=%u tls=preserved tlb-rounds=64 protection-faults=128 "
           "jobs=%u split=%u/%u sum=%llu mask=%x destructors=2 cond-timeout=checked\n",
           migrations, JOBS, jobs[0], jobs[1], (unsigned long long)expected, cpu_mask);
    return 0;
}
