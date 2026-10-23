/* Native libc boundary regressions: errors, descriptor lifetime, mappings,
 * stream aliases, argv/envp and waiting for whichever child exits first. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "../liba20rt/a20_task.h"
#include "../liba20rt/a20_handle.h"

extern const a20_start_info_t *__a20_start_info;

static int failures;
static const char *self;

static void check(const char *name, int condition) {
    if (!condition) {
        printf("MLIBC_SEMANTICS: FAIL %s errno=%d\n", name, errno);
        failures++;
    }
}

static void reap(pid_t pid, const char *name) {
    int status = -1;
    check(name, pid > 0 && waitpid(pid, &status, 0) == pid &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void errors(void) {
    errno = 0;
    check("no-child", waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    errno = 0;
    check("dup2-invalid-same", dup2(63, 63) == -1 && errno == EBADF);
    errno = 0;
    check("dup2-negative", dup2(1, -1) == -1 && errno == EBADF);
    int one = 1;
    errno = 0;
    check("setsockopt-invalid",
          setsockopt(-1, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == -1 &&
              errno == EBADF);
    struct termios ti = {0};
    errno = 0;
    check("tcsetattr-invalid",
          tcsetattr(-1, TCSANOW, &ti) == -1 && errno == EBADF);
}

static void socket_options(void) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    check("socket-create", socket_fd >= 0);
    if (socket_fd < 0)
        return;
    int one = 1, value = -1;
    socklen_t length = sizeof(value);
    check("socket-reuseaddr-set",
          setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ==
              0);
    check("socket-reuseaddr-get",
          getsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &value, &length) ==
                  0 &&
              value == 1 && length == sizeof(value));
    length = sizeof(value);
    check("socket-type",
          getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &value, &length) == 0 &&
              value == SOCK_STREAM);
    check("socket-nodelay-set", setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
                                           &one, sizeof(one)) == 0);
    length = sizeof(value);
    check("socket-nodelay-get", getsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
                                           &value, &length) == 0 &&
                                    value == 1);
    int alias = dup(socket_fd);
    value = 0;
    check("socket-alias-set",
          alias >= 0 && setsockopt(alias, SOL_SOCKET, SO_REUSEADDR, &value,
                                   sizeof(value)) == 0);
    length = sizeof(value);
    value = -1;
    check("socket-alias-shared", getsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                                            &value, &length) == 0 &&
                                     value == 0);
    length = sizeof(value);
    errno = 0;
    check("socket-unknown-option",
          getsockopt(socket_fd, SOL_SOCKET, 0x7fff, &value, &length) == -1 &&
              errno == ENOPROTOOPT);
    close(alias);
    close(socket_fd);
    length = sizeof(value);
    errno = 0;
    check("socket-wrong-object",
          getsockopt(1, SOL_SOCKET, SO_TYPE, &value, &length) == -1 &&
              errno == ENOTSOCK);
    int pair[2];
    if (!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair)) {
        char byte = 0;
        check("socketpair-write", write(pair[0], "z", 1) == 1);
        check("socketpair-read", read(pair[1], &byte, 1) == 1 && byte == 'z');
        close(pair[0]);
        check("socketpair-last-close-eof", read(pair[1], &byte, 1) == 0);
        close(pair[1]);
    } else
        check("socketpair-create", 0);
    int recreated = 0;
    for (; recreated < 1100; recreated++) {
        int temporary = socket(AF_INET, SOCK_DGRAM, 0);
        if (temporary < 0 || close(temporary))
            break;
    }
    check("socket-create-close-reclaims-resources", recreated == 1100);

    int protocol_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (protocol_socket >= 0) {
        int protocol = -1;
        socklen_t size = sizeof(protocol);
        check("socket-default-protocol",
              getsockopt(protocol_socket, SOL_SOCKET, SO_PROTOCOL, &protocol,
                         &size) == 0 &&
                  protocol == IPPROTO_TCP);
        close(protocol_socket);
    } else
        check("socket-default-protocol-create", 0);
}

static void terminals(void) {
    struct termios saved, changed, actual;
    struct winsize size, adjusted, restored;
    check("tty-console", isatty(0) == 1 && tcgetattr(0, &saved) == 0);
    changed = saved;
    changed.c_lflag ^= ECHO;
    changed.c_cc[VERASE] = 8;
    int alias = dup(0);
    check("tty-alias", alias >= 0 && isatty(alias) == 1);
    check("tty-set-state", tcsetattr(alias, TCSANOW, &changed) == 0);
    check("tty-shared-state", tcgetattr(0, &actual) == 0 &&
                                  actual.c_lflag == changed.c_lflag &&
                                  actual.c_cc[VERASE] == 8);
    check("tty-restore", tcsetattr(0, TCSANOW, &saved) == 0);
    check("tty-window-get", ioctl(0, TIOCGWINSZ, &size) == 0);
    adjusted = size;
    adjusted.ws_row = 37;
    adjusted.ws_col = 111;
    check("tty-window-set", ioctl(alias, TIOCSWINSZ, &adjusted) == 0);
    check("tty-window-shared", ioctl(0, TIOCGWINSZ, &restored) == 0 &&
                                   restored.ws_row == 37 &&
                                   restored.ws_col == 111);
    check("tty-window-restore", ioctl(0, TIOCSWINSZ, &size) == 0);
    close(alias);
    int fd = open("/mlibc-not-a-tty", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check("tty-plain-file", fd >= 0);
    errno = 0;
    check("tty-file-get", tcgetattr(fd, &actual) == -1 && errno == ENOTTY);
    errno = 0;
    check("tty-file-isatty", isatty(fd) == 0 && errno == ENOTTY);
    errno = 0;
    check("tty-file-winsize",
          ioctl(fd, TIOCGWINSZ, &restored) == -1 && errno == ENOTTY);
    close(fd);
    unlink("/mlibc-not-a-tty");
    int pipefd[2];
    if (!pipe(pipefd)) {
        int redirected = dup(0);
        check("tty-redirect", dup2(pipefd[0], 0) == 0);
        errno = 0;
        check("tty-redirect-isatty", isatty(0) == 0 && errno == ENOTTY);
        check("tty-redirect-restore", dup2(redirected, 0) == 0);
        close(redirected);
        close(pipefd[0]);
        close(pipefd[1]);
    } else
        check("tty-pipe", 0);
    check("terminal-ioctl-flush-scalar", ioctl(1, TCFLSH, TCIOFLUSH) == 0);
}

static void streams(void) {
    int p[2];
    if (pipe2(p, O_NONBLOCK)) {
        check("pipe-create", 0);
        return;
    }
    check("pipe-write", write(p[1], "abcd", 4) == 4);
    char c = 0;
    check("pipe-first-byte", read(p[0], &c, 1) == 1 && c == 'a');
    int alias = dup(p[0]);
    check("pipe-alias-byte", alias >= 0 && read(alias, &c, 1) == 1 && c == 'b');
    check("pipe-original-byte", read(p[0], &c, 1) == 1 && c == 'c');
    check("pipe-alias-last-byte", read(alias, &c, 1) == 1 && c == 'd');
    check("pipe-alias-status-flags", fcntl(alias, F_SETFL, 0) == 0 &&
                                         !(fcntl(p[0], F_GETFL) & O_NONBLOCK));
    check("pipe-alias-nonblock", fcntl(alias, F_SETFL, O_NONBLOCK) == 0 &&
                                     (fcntl(p[0], F_GETFL) & O_NONBLOCK));
    errno = 0;
    check("pipe-empty", read(alias, &c, 1) == -1 && errno == EAGAIN);
    errno = 0;
    check("pipe-direction", read(p[1], &c, 1) == -1 && errno == EBADF);
    close(p[1]);
    check("pipe-eof", read(alias, &c, 1) == 0);
    close(alias);
    close(p[0]);
}

static volatile sig_atomic_t signal_seen;

static void signal_handler(int signal) {
    (void)signal;
    signal_seen++;
}

static void signal_waits(void) {
    struct sigaction action = {.sa_handler = signal_handler}, old_action;
    sigemptyset(&action.sa_mask);
    check("signal-action", sigaction(SIGUSR1, &action, &old_action) == 0);
    sigset_t blocked, saved, empty, current;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigemptyset(&empty);
    check("signal-mask", sigprocmask(SIG_BLOCK, &blocked, &saved) == 0);
    check("signal-before-wait", kill(getpid(), SIGUSR1) == 0);
    struct timespec limit = {.tv_sec = 1};
    errno = 0;
    check("ppoll-mask-and-interrupt", ppoll(NULL, 0, &limit, &empty) == -1 &&
                                          errno == EINTR && signal_seen == 1);
    check("ppoll-restores-mask",
          sigprocmask(SIG_SETMASK, NULL, &current) == 0 &&
              sigismember(&current, SIGUSR1) == 1);
    check("signal-before-pselect", kill(getpid(), SIGUSR1) == 0);
    errno = 0;
    check("pselect-mask-and-interrupt",
          pselect(0, NULL, NULL, NULL, &limit, &empty) == -1 &&
              errno == EINTR && signal_seen == 2);
    check("pselect-restores-mask",
          sigprocmask(SIG_SETMASK, NULL, &current) == 0 &&
              sigismember(&current, SIGUSR1) == 1);
    sigprocmask(SIG_SETMASK, &saved, NULL);
    sigaction(SIGUSR1, &old_action, NULL);
}

static void wait_interrupts(void) {
    struct sigaction action = {.sa_handler = signal_handler}, saved;
    sigemptyset(&action.sa_mask);
    check("wait-signal-action", sigaction(SIGUSR1, &action, &saved) == 0);
    for (unsigned restart = 0; restart < 2; restart++) {
        action.sa_flags = restart ? SA_RESTART : 0;
        sigaction(SIGUSR1, &action, NULL);
        pid_t child = fork();
        if (!child) {
            struct timespec delay = {.tv_nsec = 200000000};
            nanosleep(&delay, NULL);
            _exit(0);
        }
        check("wait-signal-child", child > 0);
        if (child <= 0)
            continue;
        check("wait-signal-enqueue", kill(getpid(), SIGUSR1) == 0);
        int status = -1;
        errno = 0;
        pid_t result = waitpid(-1, &status, 0);
        if (restart)
            check("wait-any-restarts-handler", result == child &&
                                                   WIFEXITED(status) &&
                                                   WEXITSTATUS(status) == 0);
        else {
            check("wait-any-interrupted-handler",
                  result == -1 && errno == EINTR);
            if (result == -1)
                reap(child, "wait-any-after-interrupt");
        }
    }
    sigaction(SIGUSR1, &saved, NULL);
    struct timespec resolution;
    errno = 0;
    check("clock-resolution-invalid",
          clock_getres((clockid_t)-1, &resolution) == -1 && errno == EINVAL);
    check("clock-resolution-real",
          clock_getres(CLOCK_MONOTONIC, &resolution) == 0 &&
              resolution.tv_nsec > 0 && resolution.tv_nsec < 1000000000);
}

static void mappings(void) {
    const char *path = "/mlibc-semantics-map";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        check("map-file-create", 0);
        return;
    }
    check("map-file-size", ftruncate(fd, 4096) == 0 && write(fd, "a", 1) == 1);
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check("map-shared", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        p[0] = 'b';
        char c = 0;
        check("map-shared-visible", pread(fd, &c, 1, 0) == 1 && c == 'b');
        check("map-shared-unmap", munmap(p, 4096) == 0);
    }
    close(fd);
    fd = open(path, O_RDONLY);
    p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check("map-private-readonly-source", p != MAP_FAILED);
    if (p != MAP_FAILED) {
        p[0] = 'c';
        char c = 0;
        check("map-private-no-writeback", pread(fd, &c, 1, 0) == 1 && c == 'b');
        munmap(p, 4096);
    }
    close(fd);
    unlink(path);
    for (int shared = 0; shared < 2; shared++) {
        int mode = shared ? MAP_SHARED : MAP_PRIVATE;
        p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, mode | MAP_ANONYMOUS, -1,
                 0);
        check("map-anonymous", p != MAP_FAILED);
        if (p == MAP_FAILED)
            continue;
        p[0] = 'a';
        pid_t pid = fork();
        if (!pid) {
            p[0] = 'z';
            _exit(0);
        }
        reap(pid, "map-fork-child");
        check(shared ? "map-anon-shared-fork" : "map-anon-private-fork",
              p[0] == (shared ? 'z' : 'a'));
        munmap(p, 4096);
    }
}

struct fd_growth_reader {
    int fd, *go, *done, failed;
    unsigned calls;
};
static void *fd_growth_read(void *opaque) {
    struct fd_growth_reader *reader = opaque;
    while (!__atomic_load_n(reader->go, __ATOMIC_ACQUIRE))
        sched_yield();
    while (!__atomic_load_n(reader->done, __ATOMIC_ACQUIRE)) {
        char data[6];
        if (pread(reader->fd, data, 6, 0) != 6 || memcmp(data, "stable", 6) ||
            (fcntl(reader->fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
            reader->failed = 1;
            break;
        }
        reader->calls++;
    }
    return NULL;
}
static void descriptor_growth(void) {
    int fd = open("/mlibc-fd-growth", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        check("fd-growth-file", 0);
        return;
    }
    check("fd-growth-data", write(fd, "stable", 6) == 6);
    int go = 0, done = 0, files[192];
    struct fd_growth_reader readers[2] = {{fd, &go, &done, 0, 0},
                                          {fd, &go, &done, 0, 0}};
    pthread_t threads[2];
    unsigned started = 0;
    for (; started < 2; started++)
        if (pthread_create(&threads[started], NULL, fd_growth_read,
                           &readers[started]))
            break;
    check("fd-growth-readers", started == 2);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    unsigned opened = 0;
    for (; opened < 192; opened++) {
        files[opened] = open("/dev/null", O_RDONLY);
        if (files[opened] < 0)
            break;
    }
    check("fd-growth-open-many", opened == 192);
    struct timespec delay = {.tv_nsec = 20000000};
    nanosleep(&delay, NULL);
    for (unsigned i = 0; i < opened; i++)
        close(files[i]);
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
        check("fd-growth-readers-stable",
              !readers[i].failed && readers[i].calls);
    }
    close(fd);
    unlink("/mlibc-fd-growth");
}

struct positional_job {
    int fd;
    unsigned index;
    int failed;
};

static void *positional_worker(void *argument) {
    struct positional_job *job = argument;
    const char *expected = job->index ? "89abcdef" : "01234567";
    char buffer[8];
    for (unsigned i = 0; i < 2000; i++) {
        if (pread(job->fd, buffer, sizeof(buffer), job->index * 8) != 8 ||
            memcmp(buffer, expected, 8) ||
            pwrite(job->fd, expected, 8, 64 + job->index * 16) != 8) {
            job->failed = 1;
            break;
        }
    }
    return NULL;
}

static void positional_io(void) {
    int fd =
        open("/mlibc-semantics-positional", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        check("positional-open", 0);
        return;
    }
    check("positional-initial", write(fd, "0123456789abcdefXY", 18) == 18);
    struct positional_job jobs[2] = {{fd, 0, 0}, {fd, 1, 0}};
    pthread_t threads[2];
    unsigned started = 0;
    for (; started < 2; started++)
        if (pthread_create(&threads[started], NULL, positional_worker,
                           &jobs[started]))
            break;
    check("positional-workers-created", started == 2);
    for (unsigned i = 0; i < 2000; i++) {
        char c = 0;
        if (lseek(fd, 17, SEEK_SET) != 17 || read(fd, &c, 1) != 1 || c != 'Y') {
            check("positional-shared-offset", 0);
            break;
        }
    }
    for (unsigned i = 0; i < started; i++) {
        check("positional-join", pthread_join(threads[i], NULL) == 0);
        check("positional-data", !jobs[i].failed);
    }
    check("positional-keeps-offset", lseek(fd, 0, SEEK_CUR) == 18);
    close(fd);
    unlink("/mlibc-semantics-positional");
}

struct stream_job {
    int fd;
    unsigned index;
    int failed;
};

static void *stream_writer(void *argument) {
    struct stream_job *job = argument;
    char data[4096];
    memset(data, 'A' + job->index, sizeof(data));
    for (unsigned i = 0; i < 32; i++)
        if (write(job->fd, data, sizeof(data)) != sizeof(data)) {
            job->failed = 1;
            break;
        }
    return NULL;
}

static void atomic_streams(void) {
    int p[2];
    if (pipe(p)) {
        check("atomic-pipe", 0);
        return;
    }
    struct stream_job jobs[2] = {{p[1], 0, 0}, {p[1], 1, 0}};
    pthread_t threads[2];
    unsigned started = 0, records[2] = {0};
    for (; started < 2; started++)
        if (pthread_create(&threads[started], NULL, stream_writer,
                           &jobs[started]))
            break;
    check("atomic-workers-created", started == 2);
    for (unsigned record = 0; record < started * 32; record++) {
        char data[4096];
        ssize_t n = read(p[0], data, sizeof(data));
        if (n != sizeof(data) || data[0] < 'A' || data[0] > 'B') {
            check("atomic-pipe-record", 0);
            close(p[0]);
            p[0] = -1;
            break;
        }
        records[data[0] - 'A']++;
        for (unsigned i = 1; i < sizeof(data); i++)
            if (data[i] != data[0]) {
                check("atomic-pipe-no-interleaving", 0);
                break;
            }
    }
    for (unsigned i = 0; i < started; i++) {
        check("atomic-join", pthread_join(threads[i], NULL) == 0);
        check("atomic-writer", !jobs[i].failed && records[i] == 32);
    }
    close(p[0]);
    close(p[1]);
}

/* Exercise the old manifest and child-registry limits with real kernel I/O,
 * since a copied libc fd number alone cannot prove a capability survived. */
static void vector_io(void) {
    int pair[2];
    if (pipe(pair)) {
        check("vector-pipe", 0);
        return;
    }
    struct iovec source[2] = {{"ab", 2}, {"cde", 3}};
    char first[2] = {0}, second[3] = {0};
    struct iovec destination[2] = {{first, 2}, {second, 3}};
    check("vector-write", writev(pair[1], source, 2) == 5);
    check("vector-read", readv(pair[0], destination, 2) == 5 &&
                             !memcmp(first, "ab", 2) &&
                             !memcmp(second, "cde", 3));
    check("vector-empty",
          readv(pair[0], NULL, 0) == 0 && writev(pair[1], NULL, 0) == 0);
    errno = 0;
    check("vector-negative",
          writev(pair[1], source, -1) == -1 && errno == EINVAL);
    errno = 0;
    check("vector-excess-count",
          readv(pair[0], destination, 1025) == -1 && errno == EINVAL);
    errno = 0;
    check("vector-invalid-pointer",
          writev(pair[1], (const struct iovec *)(uintptr_t)1, 1) == -1 &&
              errno == EFAULT);
    struct iovec overflow[2] = {{first, INT64_MAX}, {second, 1}};
    errno = 0;
    check("vector-overflow",
          writev(pair[1], overflow, 2) == -1 && errno == EINVAL);
    close(pair[0]);
    close(pair[1]);
    int fd = open("/mlibc-vector-file", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check("vector-file", fd >= 0 && write(fd, "abcdef", 6) == 6 &&
                             lseek(fd, 3, SEEK_SET) == 3);
    char data[6] = {0};
    struct iovec pieces[2] = {{data, 2}, {data + 2, 2}};
    check("vector-pread",
          preadv(fd, pieces, 2, 2) == 4 && !memcmp(data, "cdef", 4));
    check("vector-pread-keeps-offset", lseek(fd, 0, SEEK_CUR) == 3);
    pieces[0] = (struct iovec){"XY", 2};
    pieces[1] = (struct iovec){"Z", 1};
    check("vector-pwrite",
          pwritev(fd, pieces, 2, 1) == 3 && lseek(fd, 0, SEEK_CUR) == 3);
    check("vector-pwrite-data",
          pread(fd, data, 6, 0) == 6 && !memcmp(data, "aXYZef", 6));
    close(fd);
    unlink("/mlibc-vector-file");
}
static a20_status_t native_wait_retry(a20_handle_t task,
                                      a20_task_status_t *status) {
    a20_status_t result;
    do {
        result = a20_task_wait(task, 0, status);
        if (result == -A20_ERR_INTERRUPTED) {
            a20_signal_control_t signals = {.size = sizeof(signals),
                                            .version = 1,
                                            .operation =
                                                A20_SIGNAL_CONTROL_CHECK,
                                            .flags = A20_SIGNAL_THREAD};
            a20_status_t taken = a20_thread_signal_control(&signals);
            if (taken < 0)
                return taken;
        }
    } while (result == -A20_ERR_INTERRUPTED);
    return result;
}
static void raw_native_thread(void *unused) {
    (void)unused;
    for (;;)
        __asm__ volatile("" ::: "memory");
}
static void native_thread_capability(void) {
    void *stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("native-thread-stack", stack != MAP_FAILED);
    if (stack == MAP_FAILED)
        return;
    a20_thread_create_args_t create = {.size = sizeof(create),
                                       .version = 1,
                                       .entry = (uint64_t)raw_native_thread,
                                       .stack_base = (uint64_t)stack + 65536,
                                       .stack_size = 16,
                                       .flags = A20_THREAD_CREATE_JOINABLE,
                                       .out_thread = A20_HANDLE_NULL};
    a20_status_t result = a20_thread_create(&create);
    check("native-thread-created",
          result >= 0 && create.out_thread != A20_HANDLE_NULL);
    if (result >= 0) {
        check("thread-cap-notification",
              a20_syscall6(A20_SYS_task_kill, create.out_thread, SIGUSR1, 0, 0,
                           0, 0) == 0);
        a20_signal_control_t pending = {.size = sizeof(pending),
                                        .version = 1,
                                        .operation = A20_SIGNAL_CONTROL_CHECK,
                                        .flags = A20_SIGNAL_THREAD |
                                                 A20_SIGNAL_PEEK};
        check("thread-cap-cannot-broadcast",
              a20_thread_signal_control(&pending) == 0 &&
                  !(pending.result & (1ULL << SIGUSR1)));
        check("thread-cap-terminate",
              a20_syscall6(A20_SYS_task_kill, create.out_thread, SIGKILL, 0, 0,
                           0, 0) == 0);
        a20_task_status_t status = {0};
        a20_status_t waited;
        do {
            waited = a20_task_wait(create.out_thread, 0, &status);
            if (waited == -A20_ERR_INTERRUPTED) {
                pending.flags = A20_SIGNAL_THREAD;
                a20_thread_signal_control(&pending);
            }
        } while (waited == -A20_ERR_INTERRUPTED);
        if (waited || status.exit_reason != A20_TASK_EXIT_SIGNAL)
            fprintf(
                stderr,
                "MLIBC_SEMANTICS: native thread wait=%lld reason=%u code=%d\n",
                (long long)waited, status.exit_reason, status.exit_code);
        check("thread-cap-reap",
              waited == 0 && status.exit_reason == A20_TASK_EXIT_SIGNAL);
        a20_hdl_close(create.out_thread);
    }
    munmap(stack, 65536);
}
static void native_legacy_contract(void) {
    pid_t child = fork();
    if (!child) {
        a20_syscall6(A20_SYS_task_exit, 37, UINT64_MAX, UINT64_MAX, UINT64_MAX,
                     UINT64_MAX, UINT64_MAX);
        _exit(98);
    }
    int status = -1;
    check("native-legacy-exit-unused-registers",
          child > 0 && waitpid(child, &status, 0) == child &&
              WIFEXITED(status) && WEXITSTATUS(status) == 37);
    const char *missing = "/native-no-such-exec-image";
    char *legacy_argv[] = {(char *)missing, NULL};
    a20_status_t result =
        a20_syscall6(A20_SYS_execve, (uint64_t)missing, (uint64_t)legacy_argv,
                     0, UINT64_MAX, UINT64_MAX, UINT64_MAX);
    check("native-legacy-exec-unused-registers", result == -A20_ERR_NO_ENTRY);
    check("native-startup-version",
          __a20_start_info && __a20_start_info->version >= 2);
    a20_terminal_config_t terminal = {.size = sizeof(terminal), .version = 1};
    terminal.reserved_byte = 1;
    check("native-terminal-reserved-byte",
          a20_syscall6(A20_SYS_handle_control, __a20_start_info->stdin_handle,
                       A20_HANDLE_CTRL_GET_TERMINAL_CONFIG, (uint64_t)&terminal,
                       0, 0, 0) == -A20_ERR_INVALID_ARGUMENT);
    terminal.reserved_byte = 0;
    for (unsigned i = 0; i < sizeof(terminal.reserved_tail); i++) {
        terminal.reserved_tail[i] = 1;
        check("native-terminal-reserved-tail",
              a20_syscall6(
                  A20_SYS_handle_control, __a20_start_info->stdin_handle,
                  A20_HANDLE_CTRL_GET_TERMINAL_CONFIG, (uint64_t)&terminal, 0,
                  0, 0) == -A20_ERR_INVALID_ARGUMENT);
        terminal.reserved_tail[i] = 0;
    }
    a20_socket_config_t config = {.size = sizeof(config),
                                  .version = 1,
                                  .fields = A20_SOCKET_OPTIONS,
                                  .option_mask = A20_SOCKET_REUSE_ADDRESS |
                                                 A20_SOCKET_REUSE_PORT};
    a20_status_t socket_handle =
        a20_syscall6(A20_SYS_net_socket, AF_INET, SOCK_STREAM, 0, 0, 0, 0);
    check("native-config-test-socket", socket_handle >= 0);
    if (socket_handle >= 0) {
        check("native-config-groups-rejected",
              a20_syscall6(A20_SYS_handle_control, socket_handle,
                           A20_HANDLE_CTRL_SET_SOCKET_CONFIG, (uint64_t)&config,
                           0, 0, 0) == -A20_ERR_INVALID_ARGUMENT);
        config.option_mask = A20_SOCKET_REUSE_ADDRESS;
        config.options = A20_SOCKET_REUSE_ADDRESS;
        check("native-config-setting",
              a20_syscall6(A20_SYS_handle_control, socket_handle,
                           A20_HANDLE_CTRL_SET_SOCKET_CONFIG, (uint64_t)&config,
                           0, 0, 0) == 0);
        config.options = 0;
        check("native-config-query",
              a20_syscall6(A20_SYS_handle_control, socket_handle,
                           A20_HANDLE_CTRL_GET_SOCKET_CONFIG, (uint64_t)&config,
                           0, 0, 0) == 0 &&
                  config.options == A20_SOCKET_REUSE_ADDRESS);
        check("native-sdk-label-typed",
              a20_hdl_set_label(socket_handle, 0) == 0);
        config.version = 2;
        check("native-config-version-rejected",
              a20_syscall6(A20_SYS_handle_control, socket_handle,
                           A20_HANDLE_CTRL_GET_SOCKET_CONFIG, (uint64_t)&config,
                           0, 0, 0) == -A20_ERR_INVALID_ARGUMENT);
        check("clone-budget-set",
              a20_hdl_set_temporal(socket_handle, 0, 8,
                                   A20_TEMPORAL_OP_COUNT) == 0);
        a20_handle_temporal_args_t before;
        check("clone-budget-source",
              a20_hdl_get_temporal(socket_handle, &before) == 0);
        a20_clone_handle_t transfer = {.parent_handle = socket_handle};
        a20_clone_args_t clone = {.size = sizeof(clone),
                                  .version = 1,
                                  .flags = A20_CLONE_COW_VM,
                                  .root_dir = __a20_start_info->root_dir,
                                  .cwd_dir = __a20_start_info->cwd_dir,
                                  .handles = (uint64_t)&transfer,
                                  .handle_count = 1};
        a20_status_t child = a20_task_clone(&clone);
        if (!child) {
            a20_handle_temporal_args_t after;
            int failed =
                a20_hdl_get_temporal(transfer.child_handle, &after) != 0 ||
                after.temporal_flags != A20_TEMPORAL_OP_COUNT ||
                after.expiry_ns || after.remaining_ops >= before.remaining_ops;
            if (a20_hdl_set_temporal(transfer.child_handle, 0, 100,
                                     A20_TEMPORAL_OP_COUNT) != -A20_ERR_ACCESS)
                failed = 1;
            a20_task_exit(failed ? 89 : 0);
        }
        check("clone-budget-child", child > 0);
        if (child > 0) {
            a20_task_status_t status;
            check("clone-budget-never-refreshes",
                  native_wait_retry(clone.out_task, &status) == 0 &&
                      status.exit_reason == 0 && status.exit_code == 0);
            a20_hdl_close(clone.out_task);
        }
        a20_hdl_close(socket_handle);
    }
}
static void signal_termination(void) {
    int pair[2], status = -1;
    if (pipe(pair)) {
        check("sigpipe-create", 0);
        return;
    }
    close(pair[0]);
    pid_t child = fork();
    if (!child) {
        write(pair[1], "x", 1);
        _exit(94);
    }
    check("sigpipe-default-status",
          child > 0 && waitpid(child, &status, 0) == child &&
              WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
    struct sigaction ignored = {.sa_handler = SIG_IGN}, saved;
    sigemptyset(&ignored.sa_mask);
    check("sigpipe-ignore", sigaction(SIGPIPE, &ignored, &saved) == 0);
    errno = 0;
    check("sigpipe-ignored-error",
          write(pair[1], "x", 1) == -1 && errno == EPIPE);
    sigaction(SIGPIPE, &saved, NULL);
    close(pair[1]);
    child = fork();
    if (!child)
        _exit(128 + SIGPIPE);
    check("normal-high-exit-is-distinct",
          child > 0 && waitpid(child, &status, 0) == child &&
              WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGPIPE);
    if (!socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        close(pair[0]);
        errno = 0;
        check("send-nosignal",
              send(pair[1], "x", 1, MSG_NOSIGNAL) == -1 && errno == EPIPE);
        close(pair[1]);
    } else
        check("send-nosignal-pair", 0);
}
static void *pure_user_worker(void *opaque) {
    int fd = (int)(intptr_t)opaque;
    if (write(fd, "r", 1) != 1)
        _exit(96);
    for (;;)
        __asm__ volatile("" ::: "memory");
}
static void kill_process_group(void) {
    int pipefd[2], status = -1;
    if (pipe(pipefd)) {
        check("kill-group-pipe", 0);
        return;
    }
    pid_t child = fork();
    if (!child) {
        close(pipefd[0]);
        pthread_t worker;
        if (pthread_create(&worker, NULL, pure_user_worker,
                           (void *)(intptr_t)pipefd[1]))
            _exit(97);
        for (;;)
            __asm__ volatile("" ::: "memory");
    }
    close(pipefd[1]);
    char ready = 0;
    check("kill-group-ready",
          child > 0 && read(pipefd[0], &ready, 1) == 1 && ready == 'r');
    check("kill-group-send", child > 0 && kill(child, SIGKILL) == 0);
    check("kill-group-status",
          child > 0 && waitpid(child, &status, 0) == child &&
              WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    close(pipefd[0]);
}
struct guard_probe {
    void *base;
    int ready, release, error;
};
static void *guard_worker(void *opaque) {
    struct guard_probe *probe = opaque;
    pthread_attr_t attr;
    size_t size;
    probe->error = pthread_getattr_np(pthread_self(), &attr);
    if (!probe->error) {
        probe->error = pthread_attr_getstack(&attr, &probe->base, &size);
        pthread_attr_destroy(&attr);
    }
    __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&probe->release, __ATOMIC_ACQUIRE))
        sched_yield();
    return NULL;
}
static void stack_guard(void) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 65536);
    pthread_attr_setguardsize(&attr, 4096);
    struct guard_probe probe = {0};
    pthread_t thread;
    if (pthread_create(&thread, &attr, guard_worker, &probe)) {
        check("guard-create", 0);
        return;
    }
    while (!__atomic_load_n(&probe.ready, __ATOMIC_ACQUIRE))
        sched_yield();
    check("guard-attributes", !probe.error);
    if (!probe.error) {
        pid_t child = fork();
        if (!child) {
            *((volatile char *)probe.base - 1) = 1;
            _exit(95);
        }
        int status = -1;
        check("guard-page-protects-stack",
              child > 0 && waitpid(child, &status, 0) == child &&
                  WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
    __atomic_store_n(&probe.release, 1, __ATOMIC_RELEASE);
    check("guard-join", pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attr);
}

struct lock_job {
    int fd, failed;
};
static void *lock_worker(void *opaque) {
    struct lock_job *job = opaque;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 16};
    if (fcntl(job->fd, F_GETLK, &lock) || lock.l_type != F_UNLCK)
        job->failed = 1;
    lock = (struct flock){.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_len = 8};
    if (fcntl(job->fd, F_SETLK, &lock))
        job->failed = 1;
    return NULL;
}
static void file_locks(void) {
    const char *path = "/mlibc-lock-test";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        check("lock-open", 0);
        return;
    }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 16};
    check("lock-set", fcntl(fd, F_SETLK, &lock) == 0);
    struct lock_job job = {fd, 0};
    pthread_t worker;
    if (!pthread_create(&worker, NULL, lock_worker, &job)) {
        check("lock-thread-join", pthread_join(worker, NULL) == 0);
        check("lock-shared-by-threads", !job.failed);
    } else
        check("lock-thread-create", 0);
    pid_t parent = getpid(), child = fork();
    if (!child) {
        struct flock wanted = {
            .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 16};
        if (fcntl(fd, F_GETLK, &wanted) || wanted.l_type != F_WRLCK ||
            wanted.l_pid != parent || wanted.l_start != 8 || wanted.l_len != 8)
            _exit(85);
        wanted = (struct flock){
            .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 8, .l_len = 8};
        if (fcntl(fd, F_SETLK, &wanted) != -1 ||
            (errno != EAGAIN && errno != EACCES))
            _exit(86);
        struct sigaction action = {.sa_handler = signal_handler};
        sigemptyset(&action.sa_mask);
        signal_seen = 0;
        if (sigaction(SIGUSR1, &action, NULL) || kill(getpid(), SIGUSR1))
            _exit(87);
        if (fcntl(fd, F_SETLKW, &wanted) != -1 || errno != EINTR ||
            signal_seen != 1)
            _exit(88);
        _exit(0);
    }
    reap(child, "lock-conflict-pid-and-interruption");
    int alias = dup(fd);
    check("lock-alias-close", alias >= 0 && close(alias) == 0);
    child = fork();
    if (!child) {
        struct flock wanted = {
            .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 16};
        _exit(fcntl(fd, F_SETLK, &wanted) ? 89 : 0);
    }
    reap(child, "lock-any-close-releases-process-locks");
    lock = (struct flock){.l_type = F_WRLCK,
                          .l_whence = SEEK_SET,
                          .l_start = INT64_MAX,
                          .l_len = 2};
    errno = 0;
    check("lock-overflow",
          fcntl(fd, F_SETLK, &lock) == -1 && errno == EOVERFLOW);
    lock = (struct flock){
        .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 32, .l_len = 16};
    check("lock-ofd-set", fcntl(fd, F_OFD_SETLK, &lock) == 0);
    child = fork();
    if (!child) {
        struct flock wanted = lock;
        if (fcntl(fd, F_OFD_GETLK, &wanted) || wanted.l_type != F_UNLCK)
            _exit(90);
        int separate = open(path, O_RDWR);
        wanted = lock;
        if (separate < 0 || fcntl(separate, F_GETLK, &wanted) ||
            wanted.l_type != F_WRLCK || wanted.l_pid != -1)
            _exit(91);
        close(separate);
        _exit(0);
    }
    reap(child, "lock-ofd-inherits-description");
    close(fd);
    fd = open(path, O_RDWR);
    check("lock-last-close-releases-ofd",
          fd >= 0 && fcntl(fd, F_SETLK, &lock) == 0);
    close(fd);
    unlink(path);
}

struct thread_probe {
    void *base;
    size_t size, guard;
    int done, error;
};
static void *thread_probe_worker(void *opaque) {
    struct thread_probe *probe = opaque;
    pthread_attr_t attr;
    probe->error = pthread_getattr_np(pthread_self(), &attr);
    if (!probe->error) {
        probe->error =
            pthread_attr_getstack(&attr, &probe->base, &probe->size) ||
            pthread_attr_getguardsize(&attr, &probe->guard);
        pthread_attr_destroy(&attr);
    }
    __atomic_store_n(&probe->done, 1, __ATOMIC_RELEASE);
    return (void *)123;
}
static void thread_lifetimes(void) {
    pthread_attr_t attr;
    check("thread-attr-init", pthread_attr_init(&attr) == 0);
    check("thread-stack-size", pthread_attr_setstacksize(&attr, 65536) == 0);
    check("thread-guard-size", pthread_attr_setguardsize(&attr, 4096) == 0);
    for (unsigned detached = 0; detached < 2; detached++) {
        check("thread-detach-state",
              pthread_attr_setdetachstate(
                  &attr, detached ? PTHREAD_CREATE_DETACHED
                                  : PTHREAD_CREATE_JOINABLE) == 0);
        for (unsigned repeat = 0; repeat < 64; repeat++) {
            struct thread_probe probe = {0};
            pthread_t thread;
            int error =
                pthread_create(&thread, &attr, thread_probe_worker, &probe);
            if (error) {
                check("thread-lifetime-create", 0);
                break;
            }
            if (!detached) {
                void *result = NULL;
                check("thread-lifetime-join",
                      pthread_join(thread, &result) == 0 &&
                          result == (void *)123);
            } else {
                while (!__atomic_load_n(&probe.done, __ATOMIC_ACQUIRE))
                    sched_yield();
            }
            check("thread-attributes",
                  !probe.error && probe.guard == 4096 && probe.size == 65536);
            unsigned pending = 0;
            while (mprotect(probe.base, 4096, PROT_READ | PROT_WRITE) == 0 &&
                   pending < 1000) {
                struct timespec delay = {.tv_nsec = 2000000};
                nanosleep(&delay, NULL);
                pending++;
            }
            check(detached ? "thread-detached-stack-reclaimed"
                           : "thread-joined-stack-reclaimed",
                  pending < 1000);
        }
    }
    void *provided = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (provided != MAP_FAILED) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        check("thread-provided-stack",
              pthread_attr_setstack(&attr, provided, 65536) == 0);
        struct thread_probe probe = {0};
        pthread_t thread;
        if (!pthread_create(&thread, &attr, thread_probe_worker, &probe)) {
            check("thread-provided-join", pthread_join(thread, NULL) == 0);
            check("thread-provided-stack-kept",
                  mprotect(provided, 65536, PROT_READ | PROT_WRITE) == 0);
        } else
            check("thread-provided-create", 0);
        munmap(provided, 65536);
    } else
        check("thread-provided-map", 0);
    pthread_attr_destroy(&attr);
}

struct concurrent_waiter {
    int *go;
    pid_t target;
    pid_t result[2];
    int error[2];
    unsigned count;
};
static void *concurrent_wait(void *opaque) {
    struct concurrent_waiter *waiter = opaque;
    while (!__atomic_load_n(waiter->go, __ATOMIC_ACQUIRE))
        sched_yield();
    for (unsigned i = 0; i < waiter->count; i++) {
        int status = -1;
        errno = 0;
        waiter->result[i] = waitpid(waiter->target, &status, 0);
        waiter->error[i] = errno;
        if (waiter->result[i] > 0 &&
            (!WIFEXITED(status) || WEXITSTATUS(status)))
            waiter->error[i] = EIO;
    }
    return NULL;
}
static void concurrent_waits(void) {
    pid_t children[4];
    for (unsigned i = 0; i < 4; i++) {
        children[i] = fork();
        if (!children[i]) {
            struct timespec delay = {.tv_nsec = 50000000};
            nanosleep(&delay, NULL);
            _exit(0);
        }
        check("parallel-wait-fork", children[i] > 0);
    }
    int go = 0;
    struct concurrent_waiter waiters[2] = {{&go, -1, {0}, {0}, 2},
                                           {&go, -1, {0}, {0}, 2}};
    pthread_t threads[2];
    unsigned started = 0;
    for (; started < 2; started++)
        if (pthread_create(&threads[started], NULL, concurrent_wait,
                           &waiters[started]))
            break;
    check("parallel-wait-workers", started == 2);
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    for (unsigned i = 0; i < 4; i++) {
        unsigned found = 0;
        for (unsigned t = 0; t < started; t++)
            for (unsigned j = 0; j < 2; j++)
                if (waiters[t].result[j] == children[i] &&
                    waiters[t].error[j] != EIO)
                    found++;
        check("parallel-wait-identity-once", found == 1);
    }
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
    }
}

static void native_clone_boundary(void) {
    const a20_start_info_t *start = __a20_start_info;
    a20_clone_handle_t manifest = {start->stdin_handle, 0, 0x12345678};
    a20_clone_args_t args = {.size = sizeof(args),
                             .version = 1,
                             .flags = A20_CLONE_COW_VM,
                             .handles = (uint64_t)(uintptr_t)&manifest,
                             .handle_count = 1,
                             .root_dir = start->root_dir,
                             .cwd_dir = start->cwd_dir,
                             .out_task = A20_HANDLE_NULL,
                             .out_root = A20_HANDLE_NULL,
                             .out_cwd = A20_HANDLE_NULL,
                             .out_self = A20_HANDLE_NULL};
    a20_status_t result = a20_task_clone(&args);
    if (!result) {
        int failed = args.out_root == A20_HANDLE_NULL ||
                     args.out_cwd == A20_HANDLE_NULL ||
                     args.out_self == A20_HANDLE_NULL ||
                     args.out_task != A20_HANDLE_NULL ||
                     manifest.child_handle == 0x12345678 ||
                     manifest.child_handle == A20_HANDLE_NULL;
        a20_task_exit(failed ? 83 : 0);
        for (;;) {
        }
    }
    check("clone-parent-output", result > 0 &&
                                     args.out_task != A20_HANDLE_NULL &&
                                     args.out_root == A20_HANDLE_NULL &&
                                     manifest.child_handle == 0x12345678);
    if (result > 0) {
        a20_task_status_t status;
        check("clone-child-output",
              a20_task_wait(args.out_task, 0, &status) == 0 &&
                  status.exit_code == 0);
        a20_hdl_close(args.out_task);
    }
    args.reserved = 1;
    check("clone-reserved-rejected",
          a20_task_clone(&args) == -A20_ERR_INVALID_ARGUMENT);
    args.reserved = 0;
    manifest.parent_handle = A20_HANDLE_NULL;
    check("clone-invalid-capability-rejected",
          a20_task_clone(&args) == -A20_ERR_BAD_HANDLE);
    manifest.parent_handle = start->stdin_handle;
    manifest.child_rights = A20_RIGHT_ADMIN;
    check("clone-rights-escalation-rejected",
          a20_task_clone(&args) == -A20_ERR_ACCESS);
    manifest.child_rights = 0;
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) {
        check("clone-readonly-map", 0);
        return;
    }
    memcpy(page, &manifest, sizeof(manifest));
    check("clone-readonly-protect", mprotect(page, 4096, PROT_READ) == 0);
    args.handles = (uint64_t)(uintptr_t)page;
    for (unsigned repeat = 0; repeat < 48; repeat++) {
        result = a20_task_clone(&args);
        if (!result) {
            a20_task_exit(84);
            for (;;) {
            }
        }
        check("clone-copyout-failure-rolls-back", result == -A20_ERR_FAULT);
        if (result > 0) {
            a20_task_status_t status;
            a20_task_wait(args.out_task, 0, &status);
            a20_hdl_close(args.out_task);
        }
    }
    munmap(page, 4096);
}

static void dense_fork(void) {
    int file = open("/mlibc-dense-fork", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (file < 0) {
        check("dense-open", 0);
        return;
    }
    check("dense-write", write(file, "native", 6) == 6);
    int fds[160], count = 0;
    for (; count < 160; count++) {
        fds[count] = dup(file);
        if (fds[count] < 0)
            break;
    }
    check("dense-created", count == 160);
    pid_t child = fork();
    if (!child) {
        for (int i = 0; i < count; i++) {
            char data[6];
            if (pread(fds[i], data, 6, 0) != 6 || memcmp(data, "native", 6))
                _exit(80);
        }
        errno = 0;
        if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD)
            _exit(81);
        _exit(0);
    }
    reap(child, "dense-fork-inherits-all");
    while (count)
        close(fds[--count]);
    close(file);
    unlink("/mlibc-dense-fork");

    pid_t children[40];
    int started = 0;
    for (; started < 40; started++) {
        children[started] = fork();
        if (children[started] < 0)
            break;
        if (!children[started]) {
            errno = 0;
            if (waitpid(-1, NULL, WNOHANG) != -1 || errno != ECHILD)
                _exit(82);
            _exit(0);
        }
    }
    check("many-children-created", started == 40);
    for (int i = 0; i < started; i++)
        reap(children[i], "many-children-reaped");
}

static void exec_descriptors(void) {
    int fd = open("/mlibc-semantics-fds", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check("exec-file-create", fd >= 0);
    if (fd < 0)
        return;
    check("exec-high-dup", dup2(fd, 900) == 900);
    check("exec-cloexec-dup", dup3(fd, 901, O_CLOEXEC) == 901);
    char *args[] = {(char *)self, (char *)"fds", NULL};
    char *env[] = {NULL};
    errno = 0;
    check("failed-exec", execve("/missing-mlibc-executable", args, env) == -1);
    check("failed-exec-keeps-fds",
          fcntl(900, F_GETFD) == 0 && fcntl(901, F_GETFD) == FD_CLOEXEC);
    pid_t pid = fork();
    if (!pid) {
        if (fcntl(0, F_SETFD, FD_CLOEXEC))
            _exit(98);
        execve(self, args, env);
        _exit(99);
    }
    reap(pid, "exec-fd-inheritance");
    close(900);
    close(901);
    close(fd);
    unlink("/mlibc-semantics-fds");
}

static void arguments(void) {
    /* Include the old exact overflow boundary and counts above its limit. */
    for (int count = 248; count <= 251; count += 3) {
        pid_t pid = fork();
        if (!pid) {
            char count_text[16];
            snprintf(count_text, sizeof(count_text), "%d", count);
            char *args[253];
            args[0] = (char *)self;
            args[1] = (char *)"args";
            args[2] = count_text;
            for (int i = 3; i < count; i++)
                args[i] = (char *)"x";
            args[count] = NULL;
            char *env[] = {NULL};
            execve(self, args, env);
            _exit(99);
        }
        reap(pid, "exec-many-arguments");
    }
}

static void exec_runtime_state(void) {
    pid_t probe = fork();
    if (!probe) {
        struct sigaction ignored = {.sa_handler = SIG_IGN},
                         caught = {.sa_handler = signal_handler};
        sigemptyset(&ignored.sa_mask);
        sigemptyset(&caught.sa_mask);
        if (sigaction(SIGUSR1, &ignored, NULL) ||
            sigaction(SIGUSR2, &caught, NULL))
            _exit(91);
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &blocked, NULL))
            _exit(92);
        pid_t child = fork();
        if (!child) {
            struct timespec delay = {.tv_nsec = 100000000};
            nanosleep(&delay, NULL);
            _exit(41);
        }
        if (child < 0)
            _exit(93);
        char pid[24];
        snprintf(pid, sizeof(pid), "%d", child);
        char *args[] = {(char *)self, "exec-parent", pid, NULL};
        execv(self, args);
        _exit(94);
    }
    reap(probe, "exec-runtime-state");
}

static void waiting(void) {
    pid_t slow = fork();
    if (!slow) {
        sleep(1);
        _exit(0);
    }
    pid_t fast = fork();
    if (!fast)
        _exit(0);
    int status = -1;
    pid_t got = waitpid(-1, &status, 0);
    check("wait-any-first-finished",
          got == fast && WIFEXITED(status) && !WEXITSTATUS(status));
    if (got == fast)
        reap(slow, "wait-slow");
    else if (got == slow)
        reap(fast, "wait-fast");
}

int main(int argc, char **argv) {
    if (argc < 1)
        return 97;
    if (argc == 3 && !strcmp(argv[1], "exec-parent")) {
        struct sigaction action;
        check("exec-preserves-ignore", sigaction(SIGUSR1, NULL, &action) == 0 &&
                                           action.sa_handler == SIG_IGN);
        check("exec-resets-caught", sigaction(SIGUSR2, NULL, &action) == 0 &&
                                        action.sa_handler == SIG_DFL);
        sigset_t mask;
        check("exec-preserves-signal-mask",
              sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
                  sigismember(&mask, SIGUSR1) == 1);
        int status = -1;
        pid_t child = atoi(argv[2]);
        check("exec-preserves-child-ownership",
              waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 41);
        return failures != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "fds")) {
        check("exec-keeps-high-fd", fcntl(900, F_GETFD) == 0);
        errno = 0;
        check("exec-closes-cloexec",
              fcntl(901, F_GETFD) == -1 && errno == EBADF);
        errno = 0;
        check("exec-closes-stdio-cloexec",
              fcntl(0, F_GETFD) == -1 && errno == EBADF);
        return failures != 0;
    }
    if (argc > 2 && !strcmp(argv[1], "args"))
        return argc != atoi(argv[2]) || strcmp(argv[argc - 1], "x");
    self = argv[0];
    fprintf(stderr, "MLIBC_SEMANTICS: stage=errors\n");
    errors();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=socket_options\n");
    socket_options();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=terminals\n");
    terminals();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=streams\n");
    streams();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=signal_waits\n");
    signal_waits();
    wait_interrupts();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=mappings\n");
    mappings();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=positional_io\n");
    descriptor_growth();
    positional_io();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=atomic_streams\n");
    atomic_streams();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=vector_io\n");
    vector_io();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=signal_termination\n");
    native_legacy_contract();
    native_thread_capability();
    signal_termination();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=kill_process_group\n");
    kill_process_group();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=stack_guard\n");
    stack_guard();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=file_locks\n");
    file_locks();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=thread_lifetimes\n");
    thread_lifetimes();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=native_clone_boundary\n");
    concurrent_waits();
    native_clone_boundary();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=dense_fork\n");
    dense_fork();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=exec_descriptors\n");
    exec_descriptors();
    exec_runtime_state();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=arguments\n");
    arguments();
    fprintf(stderr, "MLIBC_SEMANTICS: stage=waiting\n");
    waiting();
    printf("MLIBC_SEMANTICS: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
