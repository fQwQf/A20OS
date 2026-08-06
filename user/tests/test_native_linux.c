/*
 * Linux personality facade test (docs/hybrid-kernel/05-idl-and-personality.md).
 *
 * Covers the object-translation layer over Native A20:
 *   1. fd table: open/create/write/seek/read/dup/close
 *   2. mmap: anonymous VMO-backed mapping, write/read, unmap
 *   3. pipe via fd table (channel-backed byte stream)
 *   4. socketpair via Native net
 *   5. futex: mismatch-return and timeout paths
 *   6. epoll-ish wait-many on a pipe read end
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/a20_linux.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t g_out = A20_HANDLE_NULL;

static void put_str(const char *s)
{
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, s, a20_strlen(s), (void *)0);
}

static int fail(const char *what)
{
    char buf[96];
    int n = 0;
    buf[n++] = 'F';
    buf[n++] = ':';
    const char *p = what;
    while (*p && n < (int)sizeof(buf) - 2)
        buf[n++] = *p++;
    buf[n++] = '\n';
    if (g_out != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_out, buf, (uint64_t)n, (void *)0);
    return 1;
}

static int fd_table_test(a20_linux_fdtable_t *t)
{
    const char *path = "/tmp/native_linux_fd.txt";
    int fd = a20_linux_open(t, path,
                            A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR |
                            A20_PATH_OPEN_TRUNC,
                            A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP |
                            A20_RIGHT_STAT | A20_RIGHT_SEEK);
    if (fd < 0)
        return fail("fd-open");

    const char *payload = "linux personality fd";
    int64_t w = a20_linux_write(t, fd, payload, a20_strlen(payload));
    if (w != (int64_t)a20_strlen(payload))
        return fail("fd-write");

    int dupfd = a20_linux_dup(t, fd);
    if (dupfd < 0)
        return fail("fd-dup");
    a20_handle_t dh = a20_linux_fd_handle(t, dupfd);
    if (dh == A20_HANDLE_NULL)
        return fail("fd-dup-handle");

    a20_hdl_seek(dh, 0, A20_SEEK_START, NULL);
    char buf[64];
    uint64_t got = 0;
    if (a20_status_is_err(a20_hdl_read_buf(dh, buf, sizeof(buf), &got)))
        return fail("fd-read");
    if (got != a20_strlen(payload) ||
        a20_memcmp(buf, payload, (size_t)got) != 0)
        return fail("fd-read-payload");

    if (a20_linux_close(t, fd) != 0 || a20_linux_close(t, dupfd) != 0)
        return fail("fd-close");
    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

static int mmap_test(void)
{
    uint64_t addr = a20_linux_mmap(2 * 4096, 3 /* RW */);
    if (addr == 0)
        return fail("mmap-alloc");
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
    p[0] = 0x42;
    p[8191] = 0x24;
    if (p[0] != 0x42 || p[8191] != 0x24)
        return fail("mmap-rw");
    if (a20_linux_munmap(addr, 2 * 4096) != 0)
        return fail("mmap-unmap");
    return 0;
}

static int pipe_test(a20_linux_fdtable_t *t)
{
    int fds[2];
    if (a20_linux_pipe(t, fds) != 0)
        return fail("pipe-create");
    const char *msg = "pipe over channel";
    a20_personality_pipe_t pipe;
    pipe.read_end = a20_linux_fd_handle(t, fds[0]);
    pipe.write_end = a20_linux_fd_handle(t, fds[1]);
    pipe.wait_queue = A20_HANDLE_NULL;
    pipe.pending_off = 0;
    pipe.pending_len = 0;
    if (a20_status_is_err(a20_personality_pipe_write(&pipe, msg,
                                                     a20_strlen(msg))))
        return fail("pipe-write");
    char buf[64];
    uint32_t len = sizeof(buf);
    if (a20_status_is_err(a20_personality_pipe_read(&pipe, buf, &len)))
        return fail("pipe-read");
    if (len != a20_strlen(msg) || a20_memcmp(buf, msg, len) != 0)
        return fail("pipe-payload");
    a20_linux_close(t, fds[0]);
    a20_linux_close(t, fds[1]);
    return 0;
}

static int socketpair_test(a20_linux_fdtable_t *t)
{
    int fds[2];
    if (a20_linux_socketpair(t, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, fds) != 0)
        return fail("socketpair-create");
    if (a20_linux_fd_handle(t, fds[0]) == A20_HANDLE_NULL ||
        a20_linux_fd_handle(t, fds[1]) == A20_HANDLE_NULL)
        return fail("socketpair-handles");
    a20_linux_close(t, fds[0]);
    a20_linux_close(t, fds[1]);
    return 0;
}

static int futex_test(void)
{
    static uint32_t word;
    word = 1;
    if (a20_linux_futex_wait(&word, 0, A20_TIMEOUT_INFINITE) != 0)
        return fail("futex-mismatch");
    word = 0;
    if (a20_linux_futex_wait(&word, 0, 5ULL * 1000 * 1000) != -110)
        return fail("futex-timeout");
    return 0;
}

static a20_personality_pipe_t g_epoll_pipe;

static void epoll_worker(uint64_t arg)
{
    (void)arg;
    a20_time_t delay = { .secs = 0, .nsecs = 20 * 1000 * 1000 };
    a20_thread_sleep(delay);
    const char msg[] = "wake";
    a20_personality_pipe_write(&g_epoll_pipe, msg, 4);
    a20_thread_exit(0);
}

static int epoll_test(void)
{
    if (a20_status_is_err(a20_personality_pipe_create(&g_epoll_pipe)))
        return fail("epoll-pipe");

    uint64_t stack;
    if (a20_status_is_err(a20_vm_alloc_pages(16, 3, &stack)))
        return fail("epoll-stack");
    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)epoll_worker;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL;
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    if (a20_thread_create(&tc) < 0)
        return fail("epoll-thread");

    a20_handle_t handles[1] = { g_epoll_pipe.read_end };
    a20_linux_epoll_event_t ev;
    a20_time_t timeout = { .secs = 1, .nsecs = 0 };
    int n = a20_linux_epoll_wait(handles, 1, &ev, 1, timeout);
    if (n != 1) {
        char dbg[48];
        int d = 0;
        const char *s = "epoll-n=";
        while (*s) dbg[d++] = *s++;
        if (n < 0) dbg[d++] = '-';
        uint32_t v = n < 0 ? (uint32_t)(-n) : (uint32_t)n;
        char t[12];
        int m = 0;
        if (v == 0) t[m++] = '0';
        while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
        while (m) dbg[d++] = t[--m];
        dbg[d++] = '\n';
        if (g_out != A20_HANDLE_NULL)
            a20_hdl_write_buf(g_out, dbg, (uint64_t)d, (void *)0);
        return fail("epoll-wait");
    }
    if (ev.handle != g_epoll_pipe.read_end ||
        !(ev.events & A20_EVENT_MASK(A20_EVENT_MESSAGE_READY)))
        return fail("epoll-event");
    a20_personality_pipe_close(&g_epoll_pipe);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;

    a20_linux_fdtable_t table;
    a20_linux_fdtable_init(&table);

    if (fd_table_test(&table) != 0)
        return 1;
    put_str("linux fd ok\n");

    if (mmap_test() != 0)
        return 1;
    put_str("linux mmap ok\n");

    if (pipe_test(&table) != 0)
        return 1;
    put_str("linux pipe ok\n");

    if (socketpair_test(&table) != 0)
        return 1;
    put_str("linux sockpair ok\n");

    if (futex_test() != 0)
        return 1;
    put_str("linux futex ok\n");

    if (epoll_test() != 0)
        return 1;
    put_str("linux epoll ok\n");

    put_str("NATIVE_LINUX: PASS\n");
    return 0;
}
