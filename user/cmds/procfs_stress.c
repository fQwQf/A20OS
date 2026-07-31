#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAP_COUNT 192

static int fail(const char *what)
{
    printf("PROCFS_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int read_all(const char *path, char **data_out, size_t *len_out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    size_t capacity = 4096;
    size_t length = 0;
    char *data = malloc(capacity + 1);
    if (!data) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    for (;;) {
        if (length == capacity) {
            capacity *= 2;
            char *new_data = realloc(data, capacity + 1);
            if (!new_data) {
                free(data);
                close(fd);
                errno = ENOMEM;
                return -1;
            }
            data = new_data;
        }
        ssize_t n = read(fd, data + length, capacity - length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            free(data);
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        length += (size_t)n;
    }
    close(fd);
    data[length] = '\0';
    *data_out = data;
    *len_out = length;
    return 0;
}

static int check_lookup_semantics(void)
{
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return fail("open self status");
    close(fd);

    fd = open("/proc/1/status", O_RDONLY);
    if (fd < 0)
        return fail("open pid 1 status");
    close(fd);

    errno = 0;
    fd = open("/proc/999999/status", O_RDONLY);
    if (fd >= 0 || errno != ENOENT) {
        if (fd >= 0)
            close(fd);
        return fail("nonexistent pid must be ENOENT");
    }

    errno = 0;
    fd = open("/proc/maps", O_RDONLY);
    if (fd >= 0 || errno != ENOENT) {
        if (fd >= 0)
            close(fd);
        return fail("root must not expose pid maps");
    }

    fd = open("/proc/self/ns/net", O_RDONLY);
    if (fd < 0)
        return fail("open self ns net");
    close(fd);
    return 0;
}

static int check_large_maps(void)
{
    void *maps[MAP_COUNT];
    for (int i = 0; i < MAP_COUNT; i++) {
        int prot = PROT_READ | ((i & 1) ? PROT_WRITE : 0);
        maps[i] = mmap(NULL, 4096, prot, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
        if (maps[i] == MAP_FAILED)
            return fail("create map set");
    }

    int fd = open("/procfs-map-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return fail("open mapped file");
    if (ftruncate(fd, 4096) < 0)
        return fail("truncate mapped file");
    void *file_map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_map == MAP_FAILED)
        return fail("map file");
    close(fd);

    char *content = NULL;
    size_t length = 0;
    if (read_all("/proc/self/maps", &content, &length) < 0)
        return fail("read large maps");
    if (length <= 4096) {
        printf("PROCFS_STRESS: maps length=%lu\n", (unsigned long)length);
        free(content);
        errno = 0;
        return fail("maps still truncated at 4 KiB");
    }
    if (!strstr(content, "/procfs-map-file")) {
        free(content);
        errno = 0;
        return fail("file mapping path after close");
    }
    free(content);

    if (read_all("/proc/self/smaps", &content, &length) < 0)
        return fail("read smaps");
    if (!strstr(content, "VmFlags:") || !strstr(content, "Rss:")) {
        free(content);
        errno = 0;
        return fail("smaps fields");
    }
    free(content);

    munmap(file_map, 4096);
    unlink("/procfs-map-file");
    for (int i = 0; i < MAP_COUNT; i++)
        munmap(maps[i], 4096);
    return 0;
}

static int check_concurrent_maps(void)
{
    int ready[2];
    if (pipe(ready) < 0)
        return fail("pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0) {
        close(ready[0]);
        write(ready[1], "r", 1);
        for (int i = 0; i < 512; i++) {
            void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED)
                munmap(p, 4096);
        }
        _exit(0);
    }

    close(ready[1]);
    char marker;
    if (read(ready[0], &marker, 1) != 1)
        return fail("child ready");
    close(ready[0]);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    for (int i = 0; i < 64; i++) {
        char *content = NULL;
        size_t length = 0;
        if (read_all(path, &content, &length) == 0)
            free(content);
        else if (errno != ENOENT && errno != ESRCH)
            return fail("concurrent maps read");
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return fail("wait child");
    return 0;
}

static int check_fdinfo(void)
{
    const char *file = "/procfs-fdinfo-file";
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return fail("open fdinfo file");
    if (write(fd, "0123456789", 10) != 10 || lseek(fd, 7, SEEK_SET) != 7)
        return fail("position fdinfo file");

    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    char *content = NULL;
    size_t length = 0;
    if (read_all(path, &content, &length) < 0)
        return fail("read fdinfo");
    unsigned long pos = 0;
    unsigned int flags = 0;
    unsigned long ino = 0;
    if (sscanf(content, "pos:\t%lu\nflags:\t%o\nino:\t%lu",
               &pos, &flags, &ino) != 3 || pos != 7 || ino == 0 ||
        !(flags & O_CLOEXEC)) {
        printf("PROCFS_STRESS: fdinfo content=%s", content);
        free(content);
        errno = 0;
        return fail("fdinfo fields");
    }
    free(content);

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    char link[128];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0)
        return fail("read fd link");
    link[n] = '\0';
    if (strcmp(link, file) != 0) {
        errno = 0;
        return fail("fd link target");
    }

    close(fd);
    unlink(file);
    errno = 0;
    if (open(path, O_RDONLY) >= 0 || errno != ENOENT)
        return fail("closed fd link must disappear");
    return 0;
}

static int check_fd_open_semantics(void)
{
    const char *file = "/procfs-fd-open-file";
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return fail("open proc fd target");
    if (write(fd, "abcdef", 6) != 6 || lseek(fd, 2, SEEK_SET) != 2)
        return fail("position proc fd target");

    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    errno = 0;
    int exclusive = open(path, O_CREAT | O_EXCL | O_WRONLY | O_TRUNC, 0600);
    if (exclusive >= 0 || errno != EEXIST) {
        if (exclusive >= 0)
            close(exclusive);
        return fail("proc fd exclusive open");
    }
    if (unlink(file) < 0)
        return fail("unlink proc fd target");

    int reopened = open(path, O_RDONLY);
    if (reopened < 0)
        return fail("open unlinked proc fd target");
    char byte = 0;
    if (read(reopened, &byte, 1) != 1 || byte != 'a') {
        close(reopened);
        errno = 0;
        return fail("proc fd independent offset");
    }
    if (lseek(fd, 0, SEEK_CUR) != 2) {
        close(reopened);
        errno = 0;
        return fail("proc fd changed source offset");
    }
    close(reopened);
    close(fd);
    return 0;
}

static int check_fd_access(void)
{
    const char *file = "/procfs-fd-access-file";
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return fail("open fd access target");
    pid_t parent = getpid();
    char fdinfo_path[64];
    snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%d/fdinfo/%d", parent,
             fd);
    char *primed = NULL;
    size_t primed_len = 0;
    if (read_all(fdinfo_path, &primed, &primed_len) < 0)
        return fail("prime fdinfo access");
    free(primed);

    pid_t child = fork();
    if (child < 0)
        return fail("fork fd access");
    if (child == 0) {
        if (setuid(1000) < 0)
            _exit(2);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/fd/%d", parent, fd);
        char link[128];
        errno = 0;
        if (readlink(path, link, sizeof(link)) >= 0 || errno != EACCES)
            _exit(3);
        errno = 0;
        int opened = open(path, O_RDONLY);
        if (opened >= 0 || errno != EACCES) {
            if (opened >= 0)
                close(opened);
            _exit(4);
        }
        struct stat st;
        errno = 0;
        if (stat(path, &st) == 0 || errno != EACCES)
            _exit(5);
        char *content = NULL;
        size_t length = 0;
        if (read_all(fdinfo_path, &content, &length) == 0) {
            free(content);
            _exit(6);
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return fail("fd access wait");
    close(fd);
    unlink(file);
    return 0;
}

static int check_concurrent_fd_reuse(void)
{
    const int watched_fd = 42;
    const char *files[] = {"/procfs-fd-a", "/procfs-fd-b"};
    int ready[2];
    if (pipe(ready) < 0)
        return fail("fd race pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("fd race fork");
    if (pid == 0) {
        close(ready[0]);
        write(ready[1], "r", 1);
        close(ready[1]);
        for (int i = 0; i < 20000; i++) {
            int fd = open(files[i & 1], O_CREAT | O_RDWR, 0600);
            if (fd < 0)
                _exit(2);
            if (dup2(fd, watched_fd) < 0)
                _exit(3);
            close(fd);
            if ((i & 3) == 0)
                close(watched_fd);
        }
        _exit(0);
    }

    close(ready[1]);
    char marker;
    if (read(ready[0], &marker, 1) != 1)
        return fail("fd race ready");
    close(ready[0]);

    char fd_path[64];
    char info_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/%d", pid, watched_fd);
    snprintf(info_path, sizeof(info_path), "/proc/%d/fdinfo/%d", pid,
             watched_fd);
    for (int i = 0; i < 512; i++) {
        char link[128];
        ssize_t n = readlink(fd_path, link, sizeof(link) - 1);
        if (n >= 0) {
            link[n] = '\0';
            if (strcmp(link, files[0]) != 0 && strcmp(link, files[1]) != 0) {
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                errno = 0;
                return fail("fd race unrelated link");
            }
        } else if (errno != ENOENT && errno != ESRCH) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return fail("fd race readlink");
        }

        char *content = NULL;
        size_t length = 0;
        if (read_all(info_path, &content, &length) == 0) {
            if (!strstr(content, "pos:\t") || !strstr(content, "flags:\t") ||
                !strstr(content, "ino:\t")) {
                free(content);
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                errno = 0;
                return fail("fd race fdinfo fields");
            }
            free(content);
        } else if (errno != ENOENT && errno != ESRCH && errno != ENOMEM) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return fail("fd race fdinfo");
        }
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return fail("fd race wait");
    unlink(files[0]);
    unlink(files[1]);
    return 0;
}

int main(void)
{
    if (check_lookup_semantics() || check_large_maps() ||
        check_concurrent_maps() || check_fdinfo() ||
        check_fd_open_semantics() ||
        check_fd_access() ||
        check_concurrent_fd_reuse())
        return 1;
    printf("PROCFS_STRESS: PASS\n");
    return 0;
}
