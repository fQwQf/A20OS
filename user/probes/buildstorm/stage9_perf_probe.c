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
extern void *mmap(void *address, size_t length, int protection, int flags,
                  int fd, off_t offset);
extern int munmap(void *address, size_t length);

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

#define FILE_COUNT 96
#define IO_ROUNDS 64
#define STAT_ROUNDS 128
#define MMAP_ROUNDS 128

static const char g_base[] = "/work/a20-stage9-perf";
static const char g_io_file[] = "/work/a20-stage9-perf/io-file";
static char g_page[4096];

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
    if (snapshot_file(label, "perf", "/proc/a20/perf") != 0)
        return -1;
    return snapshot_file(label, "task_lifetime",
                         "/proc/a20/task_lifetime");
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

static int subload_sync_cleanup(void)
{
    return cleanup_files_strict();
}

typedef int (*subload_fn_t)(void);

static int run_subload(const char *name, subload_fn_t fn)
{
    print_text("STAGE9_PERF_SUBLOAD start name=");
    print_text(name);
    print_text("\n");
    int rc = fn();
    print_text("STAGE9_PERF_SUBLOAD end name=");
    print_text(name);
    print_text(rc == 0 ? " rc=0\n" : " rc=1\n");
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
        run_subload("sync-cleanup", subload_sync_cleanup) != 0)
        goto fail;

    print_text("BUILDSTORM_STAGE9_PERF ok subloads=7 snapshots=8\n");
    return 0;

fail:
    cleanup_files_best_effort();
    print_text("BUILDSTORM_STAGE9_PERF failed\n");
    return 1;
}
