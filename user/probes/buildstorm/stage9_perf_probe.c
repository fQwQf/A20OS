typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;

struct probe_timespec {
    long tv_sec;
    long tv_nsec;
};

extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern ssize_t read(int fd, void *buf, size_t count);
extern ssize_t write(int fd, const void *buf, size_t count);
extern off_t lseek(int fd, off_t offset, int whence);
extern int ftruncate(int fd, off_t length);
extern int stat(const char *path, void *buf);
extern int mkdir(const char *path, unsigned mode);
extern int rmdir(const char *path);
extern int unlink(const char *path);
extern int fsync(int fd);
extern void sync(void);
extern int nanosleep(const struct probe_timespec *request,
                     struct probe_timespec *remaining);
extern int clock_gettime(int clock_id, struct probe_timespec *time);
extern void *mmap(void *address, size_t length, int protection, int flags,
                  int fd, off_t offset);
extern int munmap(void *address, size_t length);
extern int getpid(void);
extern int fork(void);
extern int pipe(int pipefd[2]);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define SEEK_SET 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

#define CLOCK_MONOTONIC 1
#define FILE_COUNT 768
#define IO_ROUNDS 64
#define STAT_ROUNDS 128
#define MMAP_ROUNDS 128
#define SYSCALL_ROUNDS 200000
#define FORK_ROUNDS 256
#define PIPE_PINGPONG_ROUNDS 50000
#define ANON_FAULT_BYTES (64UL * 1024UL * 1024UL)
#define ANON_FAULT_ROUNDS 4
#define FILE_FAULT_ROUNDS 8
#define FILE_FAULT_PARALLEL_ROUNDS 2
#define FILE_READ_BUFFER_BYTES (64UL * 1024UL)
#define FILE_READ_HOT_ROUNDS 4
#define CPU_FIXED_ROUNDS 50000000UL
#define CPU_PARALLEL_WORKERS 8

static const char g_base[] = "/work/a20-stage9-perf";
static const char g_io_file[] = "/work/a20-stage9-perf/io-file";
static const char g_fault_file[] = "/work/tgoskits/target/debug/tg-xtask";
static char g_page[4096];
static char g_read_buffer[FILE_READ_BUFFER_BYTES]
    __attribute__((aligned(4096)));
static volatile unsigned long g_fault_sink;

static unsigned long cpu_fixed_work(unsigned long seed)
{
    unsigned long value = seed | 1UL;

    for (unsigned long i = 0; i < CPU_FIXED_ROUNDS; i++) {
        value ^= value << 13;
        value ^= value >> 7;
        value ^= value << 17;
        value += i ^ 0x9e3779b97f4a7c15UL;
    }
    return value;
}

static size_t text_length(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    return length;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const char *cursor = (const char *)buffer;
    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written <= 0)
            return -1;
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

static void print_text(const char *text)
{
    (void)write_all(1, text, text_length(text));
}

static void print_unsigned(unsigned long value)
{
    char digits[32];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        (void)write_all(1, &digits[--count], 1);
}

static void make_file_path(unsigned index, char path[64])
{
    size_t offset = 0;
    while (g_base[offset]) {
        path[offset] = g_base[offset];
        offset++;
    }
    path[offset++] = '/';
    path[offset++] = 'f';
    path[offset++] = (char)('0' + (index / 100) % 10);
    path[offset++] = (char)('0' + (index / 10) % 10);
    path[offset++] = (char)('0' + index % 10);
    path[offset] = '\0';
}

static int snapshot_file(const char *label, const char *source,
                         const char *path)
{
    char buffer[1024];
    print_text("STAGE9_PERF_SNAPSHOT label=");
    print_text(label);
    print_text(" source=");
    print_text(source);
    print_text(" begin\n");

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        print_text("STAGE9_PERF_SNAPSHOT unavailable path=");
        print_text(path);
        print_text("\n");
        return -1;
    }
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            close(fd);
            return -1;
        }
        if (count == 0)
            break;
        if (write_all(1, buffer, (size_t)count) < 0) {
            close(fd);
            return -1;
        }
    }
    if (close(fd) != 0)
        return -1;

    print_text("STAGE9_PERF_SNAPSHOT label=");
    print_text(label);
    print_text(" source=");
    print_text(source);
    print_text(" end\n");
    return 0;
}

static int checkpoint(const char *label)
{
#ifdef A20_PORTABLE_DIAG
    (void)label;
    return 0;
#else
    if (snapshot_file(label, "perf", "/proc/a20/perf") != 0)
        return -1;
    return snapshot_file(label, "task_lifetime",
                         "/proc/a20/task_lifetime");
#endif
}

static void cleanup_files_best_effort(void)
{
    char path[64];
    for (unsigned i = 0; i < FILE_COUNT; i++) {
        make_file_path(i, path);
        (void)unlink(path);
    }
    (void)unlink(g_io_file);
    (void)rmdir(g_base);
}

static int cleanup_files_strict(void)
{
    char path[64];
    union {
        unsigned long align;
        char bytes[256];
    } stat_buffer;

    int fd = open(g_io_file, O_RDONLY);
    if (fd < 0)
        return -1;
    if (fsync(fd) != 0) {
        (void)close(fd);
        return -1;
    }
    if (close(fd) != 0)
        return -1;

    sync();
    for (unsigned i = 0; i < FILE_COUNT; i++) {
        make_file_path(i, path);
        if (unlink(path) != 0)
            return -1;
    }
    if (unlink(g_io_file) != 0 || rmdir(g_base) != 0)
        return -1;
    sync();

    for (unsigned i = 0; i < FILE_COUNT; i++) {
        make_file_path(i, path);
        if (stat(path, stat_buffer.bytes) == 0)
            return -1;
    }
    if (stat(g_io_file, stat_buffer.bytes) == 0 ||
        stat(g_base, stat_buffer.bytes) == 0)
        return -1;
    return 0;
}

static int subload_idle(void)
{
    const struct probe_timespec delay = { .tv_sec = 1, .tv_nsec = 0 };
    return nanosleep(&delay, (struct probe_timespec *)0);
}

static int subload_stat_open_close(void)
{
    union {
        unsigned long align;
        char bytes[256];
    } stat_buffer;
    const char *path = "/work/tgoskits/Cargo.toml";

    for (unsigned i = 0; i < STAT_ROUNDS; i++) {
        if (stat(path, stat_buffer.bytes) != 0)
            return -1;
        int fd = open(path, O_RDONLY);
        if (fd < 0 || close(fd) != 0)
            return -1;
    }
    return 0;
}

static int subload_write_truncate(void)
{
    int fd = open(g_io_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    for (unsigned i = 0; i < IO_ROUNDS; i++) {
        g_page[0] = (char)i;
        if (lseek(fd, 0, SEEK_SET) != 0 ||
            write_all(fd, g_page, sizeof(g_page)) != 0 ||
            ftruncate(fd, 0) != 0) {
            close(fd);
            return -1;
        }
    }
    return close(fd);
}

static int subload_ext4_create(void)
{
    char path[64];
    for (unsigned i = 0; i < FILE_COUNT; i++) {
        make_file_path(i, path);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            return -1;
        g_page[0] = (char)i;
        if (write_all(fd, g_page, sizeof(g_page)) != 0 || close(fd) != 0)
            return -1;
    }
    return 0;
}

static int subload_overwrite(void)
{
    char path[64];
    for (unsigned i = 0; i < FILE_COUNT; i++) {
        make_file_path(i, path);
        int fd = open(path, O_WRONLY);
        if (fd < 0)
            return -1;
        g_page[1] = (char)i;
        if (write_all(fd, g_page, sizeof(g_page)) != 0 || close(fd) != 0)
            return -1;
    }
    return 0;
}

static int subload_mmap(void)
{
    for (unsigned i = 0; i < MMAP_ROUNDS; i++) {
        void *mapping = mmap((void *)0, sizeof(g_page), PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED)
            return -1;
        ((volatile char *)mapping)[0] = (char)i;
        if (munmap(mapping, sizeof(g_page)) != 0)
            return -1;
    }
    return 0;
}

static int subload_syscall_getpid(void)
{
    unsigned long sum = 0;
    for (unsigned i = 0; i < SYSCALL_ROUNDS; i++)
        sum += (unsigned)getpid();
    g_fault_sink = sum;
    return 0;
}

static int subload_cpu_single(void)
{
    g_fault_sink = cpu_fixed_work(0x243f6a8885a308d3UL);
    return 0;
}

static int subload_cpu_parallel(void)
{
    int pids[CPU_PARALLEL_WORKERS];
    unsigned started = 0;

    for (unsigned worker = 0; worker < CPU_PARALLEL_WORKERS; worker++) {
        int pid = fork();
        if (pid < 0)
            break;
        if (pid == 0) {
            g_fault_sink = cpu_fixed_work(
                0x13198a2e03707344UL + (unsigned long)worker);
            _exit(0);
        }
        pids[started++] = pid;
    }

    int failed = started != CPU_PARALLEL_WORKERS;
    for (unsigned worker = 0; worker < started; worker++) {
        int status = -1;
        if (waitpid(pids[worker], &status, 0) != pids[worker] || status != 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

static int subload_fork_wait(void)
{
    for (unsigned i = 0; i < FORK_ROUNDS; i++) {
        int pid = fork();
        if (pid < 0)
            return -1;
        if (pid == 0)
            _exit(0);
        int status = -1;
        if (waitpid(pid, &status, 0) != pid || status != 0)
            return -1;
    }
    return 0;
}

static int subload_pipe_pingpong(void)
{
    int request[2] = {-1, -1};
    int response[2] = {-1, -1};
    if (pipe(request) != 0 || pipe(response) != 0)
        return -1;

    int pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char token = 0;
        (void)close(request[1]);
        (void)close(response[0]);
        for (unsigned i = 0; i < PIPE_PINGPONG_ROUNDS; i++) {
            if (read(request[0], &token, 1) != 1 ||
                write(response[1], &token, 1) != 1)
                _exit(1);
        }
        (void)close(request[0]);
        (void)close(response[1]);
        _exit(0);
    }

    (void)close(request[0]);
    (void)close(response[1]);
    char token = 1;
    int failed = 0;
    for (unsigned i = 0; i < PIPE_PINGPONG_ROUNDS; i++) {
        if (write(request[1], &token, 1) != 1 ||
            read(response[0], &token, 1) != 1) {
            failed = 1;
            break;
        }
    }
    (void)close(request[1]);
    (void)close(response[0]);
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || status != 0)
        failed = 1;
    return failed ? -1 : 0;
}

static int subload_anon_fault(void)
{
    unsigned long sum = 0;
    for (unsigned round = 0; round < ANON_FAULT_ROUNDS; round++) {
        volatile unsigned char *mapping = mmap(
            (void *)0, ANON_FAULT_BYTES, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((void *)mapping == MAP_FAILED)
            return -1;
        for (size_t offset = 0; offset < ANON_FAULT_BYTES; offset += 4096) {
            mapping[offset] = (unsigned char)(offset + round);
            sum += mapping[offset];
        }
        if (munmap((void *)mapping, ANON_FAULT_BYTES) != 0)
            return -1;
    }
    g_fault_sink = sum;
    return 0;
}

static int subload_file_fault(void)
{
    int fd = open(g_fault_file, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t end = lseek(fd, 0, 2);
    if (end <= 0 || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return -1;
    }

    size_t length = (size_t)end;
    unsigned long sum = 0;
    for (unsigned round = 0; round < FILE_FAULT_ROUNDS; round++) {
        volatile const unsigned char *mapping = mmap(
            (void *)0, length, PROT_READ, MAP_PRIVATE, fd, 0);
        if ((void *)mapping == MAP_FAILED) {
            close(fd);
            return -1;
        }
        for (size_t offset = 0; offset < length; offset += 4096)
            sum += mapping[offset];
        if (munmap((void *)mapping, length) != 0) {
            close(fd);
            return -1;
        }
    }
    g_fault_sink = sum;
    return close(fd);
}

static int subload_file_fault_parallel(void)
{
    int pids[CPU_PARALLEL_WORKERS];
    unsigned started = 0;

    for (unsigned worker = 0; worker < CPU_PARALLEL_WORKERS; worker++) {
        int pid = fork();
        if (pid < 0)
            break;
        if (pid == 0) {
            int fd = open(g_fault_file, O_RDONLY);
            if (fd < 0)
                _exit(1);
            off_t end = lseek(fd, 0, 2);
            if (end <= 0 || lseek(fd, 0, SEEK_SET) != 0)
                _exit(1);
            size_t length = (size_t)end;
            unsigned long sum = 0;
            for (unsigned round = 0;
                 round < FILE_FAULT_PARALLEL_ROUNDS; round++) {
                volatile const unsigned char *mapping = mmap(
                    (void *)0, length, PROT_READ, MAP_PRIVATE, fd, 0);
                if ((void *)mapping == MAP_FAILED)
                    _exit(1);
                for (size_t offset = 0; offset < length; offset += 4096)
                    sum += mapping[offset];
                if (munmap((void *)mapping, length) != 0)
                    _exit(1);
            }
            g_fault_sink = sum;
            _exit(close(fd) == 0 ? 0 : 1);
        }
        pids[started++] = pid;
    }

    int failed = started != CPU_PARALLEL_WORKERS;
    for (unsigned worker = 0; worker < started; worker++) {
        int status = -1;
        if (waitpid(pids[worker], &status, 0) != pids[worker] || status != 0)
            failed = 1;
    }
    return failed ? -1 : 0;
}

/* Measure the regular read(2) path after the mmap fault probes have populated
 * the canonical file page cache.  rustc and Cargo repeatedly scan large input
 * files through read/readv; this separates that VFS + user-buffer path from
 * storage latency and from executable mmap faults. */
static int subload_file_read_hot(void)
{
    int fd = open(g_fault_file, O_RDONLY);
    if (fd < 0)
        return -1;

    unsigned long sum = 0;
    for (unsigned round = 0; round < FILE_READ_HOT_ROUNDS; round++) {
        if (lseek(fd, 0, SEEK_SET) != 0) {
            close(fd);
            return -1;
        }
        for (;;) {
            ssize_t count = read(fd, g_read_buffer,
                                 sizeof(g_read_buffer));
            if (count < 0) {
                close(fd);
                return -1;
            }
            if (count == 0)
                break;
            for (size_t offset = 0; offset < (size_t)count; offset += 4096)
                sum += (unsigned char)g_read_buffer[offset];
        }
    }
    g_fault_sink = sum;
    return close(fd);
}

static int subload_sync_cleanup(void)
{
    return cleanup_files_strict();
}

typedef int (*subload_fn_t)(void);

static int run_subload(const char *name, subload_fn_t fn)
{
    struct probe_timespec started = {0};
    struct probe_timespec finished = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &started);
    print_text("STAGE9_PERF_SUBLOAD start name=");
    print_text(name);
    print_text("\n");
    int rc = fn();
    print_text("STAGE9_PERF_SUBLOAD end name=");
    print_text(name);
    print_text(rc == 0 ? " rc=0\n" : " rc=1\n");
    if (clock_gettime(CLOCK_MONOTONIC, &finished) == 0) {
        unsigned long elapsed_ms =
            (unsigned long)(finished.tv_sec - started.tv_sec) * 1000UL;
        if (finished.tv_nsec >= started.tv_nsec)
            elapsed_ms +=
                (unsigned long)(finished.tv_nsec - started.tv_nsec) / 1000000UL;
        else {
            elapsed_ms -= 1000UL;
            elapsed_ms += (unsigned long)(1000000000L + finished.tv_nsec -
                                          started.tv_nsec) / 1000000UL;
        }
        print_text("STAGE9_PERF_TIMING name=");
        print_text(name);
        print_text(" elapsed_ms=");
        print_unsigned(elapsed_ms);
        print_text("\n");
    }
    if (rc != 0)
        return rc;
    return checkpoint(name);
}

int probe_main(void)
{
    for (size_t i = 0; i < sizeof(g_page); i++)
        g_page[i] = (char)(i & 0x7f);
    cleanup_files_best_effort();
    if (mkdir(g_base, 0700) != 0)
        goto fail;

    if (checkpoint("initial") != 0 ||
        run_subload("idle", subload_idle) != 0 ||
        run_subload("stat-open-close", subload_stat_open_close) != 0 ||
        run_subload("write-truncate-4k", subload_write_truncate) != 0 ||
        run_subload("ext4-create-allocation", subload_ext4_create) != 0 ||
        run_subload("overwrite", subload_overwrite) != 0 ||
        run_subload("mmap-munmap", subload_mmap) != 0 ||
        run_subload("cpu-fixed-single", subload_cpu_single) != 0 ||
        run_subload("cpu-fixed-parallel-8", subload_cpu_parallel) != 0 ||
        run_subload("syscall-getpid", subload_syscall_getpid) != 0 ||
        run_subload("fork-wait", subload_fork_wait) != 0 ||
        run_subload("pipe-pingpong", subload_pipe_pingpong) != 0 ||
        run_subload("anon-fault", subload_anon_fault) != 0 ||
        run_subload("file-fault", subload_file_fault) != 0 ||
        run_subload("file-fault-parallel-8", subload_file_fault_parallel) != 0 ||
        run_subload("file-read-hot", subload_file_read_hot) != 0 ||
        run_subload("sync-cleanup", subload_sync_cleanup) != 0)
        goto fail;

    print_text("BUILDSTORM_STAGE9_PERF ok subloads=16 snapshots=17\n");
    return 0;

fail:
    cleanup_files_best_effort();
    print_text("BUILDSTORM_STAGE9_PERF failed\n");
    return 1;
}
