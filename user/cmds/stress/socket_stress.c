#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

#define SOCKET_STRESS_PORT  41000
#define ITERATIONS          50
#define PAYLOAD             "a20os-socket-stress-payload"
#define PAYLOAD_LEN         (sizeof(PAYLOAD) - 1)

static int fail(const char *what)
{
    printf("SOCKET_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static uint16_t bswap16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

static int tcp_server_loop(int ready_fd)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return fail("tcp-server-socket");

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = 0x0100007fU;
    addr.sin_port = bswap16(SOCKET_STRESS_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return fail("tcp-server-bind");
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return fail("tcp-server-listen");
    }
    if (ready_fd >= 0) {
        char ready = 'R';
        if (write(ready_fd, &ready, 1) != 1) {
            close(ready_fd);
            close(fd);
            return fail("tcp-server-ready");
        }
        close(ready_fd);
    }

    for (int i = 0; i < ITERATIONS; i++) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            close(fd);
            return fail("tcp-server-accept");
        }
        char buf[64];
        ssize_t n = recv(cfd, buf, sizeof(buf), 0);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(cfd);
            close(fd);
            return fail("tcp-server-recv");
        }
        if (send(cfd, PAYLOAD, PAYLOAD_LEN, 0) != PAYLOAD_LEN) {
            close(cfd);
            close(fd);
            return fail("tcp-server-send");
        }
        close(cfd);
    }

    close(fd);
    return 0;
}

static int tcp_client_loop(void)
{
    for (int i = 0; i < ITERATIONS; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return fail("tcp-client-socket");

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = 0x0100007fU;
        addr.sin_port = bswap16(SOCKET_STRESS_PORT);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            return fail("tcp-client-connect");
        }
        if (send(fd, PAYLOAD, PAYLOAD_LEN, 0) != PAYLOAD_LEN) {
            close(fd);
            return fail("tcp-client-send");
        }
        char buf[64];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(fd);
            return fail("tcp-client-recv");
        }
        close(fd);
    }
    return 0;
}

static int udp_ping_pong(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return fail("udp-fork");

    if (pid == 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            _exit(1);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = 0x0100007fU;
        addr.sin_port = bswap16(SOCKET_STRESS_PORT + 1);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(fd);
            _exit(2);
        }

        for (int i = 0; i < ITERATIONS; i++) {
            char buf[64];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
                close(fd);
                _exit(3);
            }
            if (sendto(fd, PAYLOAD, PAYLOAD_LEN, 0,
                       (struct sockaddr *)&from, fromlen) != PAYLOAD_LEN) {
                close(fd);
                _exit(4);
            }
        }
        close(fd);
        _exit(0);
    }

    usleep(200000);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fail("udp-client-socket");

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = 0x0100007fU;
    dst.sin_port = bswap16(SOCKET_STRESS_PORT + 1);

    for (int i = 0; i < ITERATIONS; i++) {
        if (sendto(fd, PAYLOAD, PAYLOAD_LEN, 0,
                   (struct sockaddr *)&dst, sizeof(dst)) != PAYLOAD_LEN) {
            close(fd);
            return fail("udp-client-sendto");
        }
        char buf[64];
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n != PAYLOAD_LEN || memcmp(buf, PAYLOAD, PAYLOAD_LEN) != 0) {
            close(fd);
            return fail("udp-client-recvfrom");
        }
    }
    close(fd);

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("udp-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("udp-server-fail");
    return 0;
}

static int concurrent_tcp_stress(void)
{
    int ready_pipe[2];
    if (pipe(ready_pipe) < 0)
        return fail("tcp-ready-pipe");

    pid_t pid = fork();
    if (pid < 0) {
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return fail("tcp-fork");
    }

    if (pid == 0) {
        close(ready_pipe[0]);
        int r = tcp_server_loop(ready_pipe[1]);
        _exit(r);
    }

    close(ready_pipe[1]);
    char ready = 0;
    ssize_t ready_n = read(ready_pipe[0], &ready, 1);
    close(ready_pipe[0]);
    if (ready_n != 1 || ready != 'R') {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        return fail("tcp-server-ready");
    }

    int r = tcp_client_loop();

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("tcp-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("tcp-server-fail");
    return r;
}

static int socket_queue_accounting(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return fail("queue-stream-socketpair");
    if (send(sv[0], "abcdefghijk", 11, 0) != 11) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-send");
    }
    int available = -1;
    if (ioctl(sv[1], FIONREAD, &available) || available != 11) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-fionread-full");
    }
    char buf[16];
    if (recv(sv[1], buf, 4, 0) != 4 || memcmp(buf, "abcd", 4)) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-partial-read");
    }
    available = -1;
    if (ioctl(sv[1], FIONREAD, &available) || available != 7) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-fionread-partial");
    }
    if (recv(sv[1], buf, sizeof(buf), 0) != 7 || memcmp(buf, "efghijk", 7)) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-read-rest");
    }
    available = -1;
    if (ioctl(sv[1], FIONREAD, &available) || available != 0) {
        close(sv[0]); close(sv[1]);
        return fail("queue-stream-fionread-empty");
    }
    close(sv[0]); close(sv[1]);

    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0) {
        if (rx >= 0) close(rx);
        if (tx >= 0) close(tx);
        return fail("queue-dgram-sockets");
    }
    struct sockaddr_in dgram_addr;
    memset(&dgram_addr, 0, sizeof(dgram_addr));
    dgram_addr.sin_family = AF_INET;
    dgram_addr.sin_addr.s_addr = 0x0100007fU;
    dgram_addr.sin_port = bswap16(SOCKET_STRESS_PORT + 2);
    if (bind(rx, (struct sockaddr *)&dgram_addr, sizeof(dgram_addr))) {
        close(rx); close(tx);
        return fail("queue-dgram-bind");
    }
    int limit = 8;
    if (setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &limit, sizeof(limit))) {
        close(rx); close(tx);
        return fail("queue-dgram-set-limit");
    }
    if (sendto(tx, "12345", 5, MSG_DONTWAIT,
               (struct sockaddr *)&dgram_addr, sizeof(dgram_addr)) != 5) {
        close(rx); close(tx);
        return fail("queue-dgram-first-send");
    }
    errno = 0;
    if (sendto(tx, "67890", 5, MSG_DONTWAIT,
               (struct sockaddr *)&dgram_addr, sizeof(dgram_addr)) != -1 ||
        errno != EAGAIN) {
        close(rx); close(tx);
        return fail("queue-dgram-byte-limit");
    }
    available = -1;
    if (ioctl(rx, FIONREAD, &available) || available != 5 ||
        recv(rx, buf, sizeof(buf), 0) != 5 || memcmp(buf, "12345", 5)) {
        close(rx); close(tx);
        return fail("queue-dgram-drop-accounting");
    }
    if (sendto(tx, "abc", 3, MSG_DONTWAIT,
               (struct sockaddr *)&dgram_addr, sizeof(dgram_addr)) != 3 ||
        ioctl(rx, FIONREAD, &available) || available != 3) {
        close(rx); close(tx);
        return fail("queue-dgram-capacity-recovered");
    }
    if (shutdown(rx, SHUT_RD)) {
        close(rx); close(tx);
        return fail("queue-shutdown-read");
    }
    available = -1;
    if (ioctl(rx, FIONREAD, &available) || available != 0) {
        close(rx); close(tx);
        return fail("queue-shutdown-detach");
    }
    close(rx); close(tx);
    puts("SOCKET_STRESS: queue-accounting PASS");
    return 0;
}

typedef struct {
    int fd;
    int ready_fd;
    int use_mmsg;
    char data[8];
    ssize_t result;
    unsigned msg_len;
} close_reuse_recv_t;

static void *close_reuse_recv_thread(void *opaque)
{
    close_reuse_recv_t *ctx = opaque;
    (void)write(ctx->ready_fd, "R", 1);
    close(ctx->ready_fd);
    struct iovec iov = { .iov_base = ctx->data, .iov_len = sizeof(ctx->data) };
    if (ctx->use_mmsg) {
        struct mmsghdr msg;
        struct timespec timeout = { .tv_sec = 2, .tv_nsec = 0 };
        memset(&msg, 0, sizeof(msg));
        msg.msg_hdr.msg_iov = &iov;
        msg.msg_hdr.msg_iovlen = 1;
        ctx->result = recvmmsg(ctx->fd, &msg, 1, 0, &timeout);
        ctx->msg_len = msg.msg_len;
    } else {
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ctx->result = recvmsg(ctx->fd, &msg, 0);
    }
    return NULL;
}

static int socket_close_reuse_pinned_recv(int use_mmsg)
{
    int old[2], replacement[2], ready[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, old) ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) || pipe(ready))
        return fail("close-reuse-setup");
    close_reuse_recv_t ctx = { .fd = old[0], .ready_fd = ready[1],
                               .use_mmsg = use_mmsg, .result = -2 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, close_reuse_recv_thread, &ctx))
        return fail("close-reuse-thread");
    char byte;
    if (read(ready[0], &byte, 1) != 1) {
        close(ready[0]);
        return fail("close-reuse-ready");
    }
    close(ready[0]);
    /* Let recvmsg enter the kernel and block before reusing its descriptor. */
    usleep(100000);
    int target_fd = old[0];
    close(target_fd);
    if (dup2(replacement[0], target_fd) != target_fd)
        return fail("close-reuse-dup2");
    close(replacement[0]);
    if (send(replacement[1], "new", 3, 0) != 3 ||
        send(old[1], "old", 3, 0) != 3)
        return fail("close-reuse-wake");
    if (pthread_join(thread, NULL))
        return fail("close-reuse-join");
    close(target_fd); close(replacement[1]); close(old[1]);
    if (ctx.result != (use_mmsg ? 1 : 3) ||
        (use_mmsg && ctx.msg_len != 3) || memcmp(ctx.data, "old", 3))
        return fail("close-reuse-recv-redirected");
    puts(use_mmsg ? "SOCKET_STRESS: close-reuse-pinned-recvmmsg PASS" :
                    "SOCKET_STRESS: close-reuse-pinned-recvmsg PASS");
    return 0;
}

typedef struct delayed_marker {
    int fd;
    ssize_t sent;
} delayed_marker_t;

static void *send_delayed_marker(void *arg)
{
    delayed_marker_t *ctx = arg;
    sleep(1);
    ctx->sent = send(ctx->fd, "m", 1, 0);
    return NULL;
}

/* Verify the SO_RCVTIMEO getter and the retained timeout behavior: the
 * socket's 100ms timeout must
 * win before a helper sends after one second. The recvmmsg call uses a much
 * longer per-call timeout, so leaking that deadline into the socket would
 * consume the marker and fail rather than hanging indefinitely. */
static int recvmmsg_socket_timeout_preserved(int recv_fd, int send_fd,
                                             const char *failure)
{
    delayed_marker_t marker = { .fd = send_fd, .sent = -2 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, send_delayed_marker, &marker))
        return fail("recvmmsg-timeout-marker-thread");
    char byte = 0;
    errno = 0;
    ssize_t n = recv(recv_fd, &byte, 1, 0);
    int recv_errno = errno;
    if (pthread_join(thread, NULL))
        return fail("recvmmsg-timeout-marker-join");
    if (n != -1 || (recv_errno != EAGAIN && recv_errno != EWOULDBLOCK) ||
        marker.sent != 1) {
        errno = recv_errno;
        return fail(failure);
    }
    errno = 0;
    if (recv(recv_fd, &byte, 1, MSG_DONTWAIT) != 1 || byte != 'm')
        return fail("recvmmsg-timeout-marker-drain");
    return 0;
}

static int socket_timeout_getter_roundtrip(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return fail("timeout-getter-socket");
    struct timeval requested[2] = {
        { .tv_sec = 2, .tv_usec = 250000 },
        { .tv_sec = 1, .tv_usec = 500000 },
    };
    struct timeval observed;
    socklen_t len;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &requested[0],
                   sizeof(requested[0])) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &requested[1],
                   sizeof(requested[1]))) {
        close(fd);
        return fail("timeout-getter-set");
    }
    len = sizeof(observed);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &observed, &len) ||
        len != sizeof(observed) || observed.tv_sec != requested[0].tv_sec ||
        observed.tv_usec != requested[0].tv_usec) {
        close(fd);
        return fail("timeout-getter-recv");
    }
    len = sizeof(observed);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &observed, &len) ||
        len != sizeof(observed) || observed.tv_sec != requested[1].tv_sec ||
        observed.tv_usec != requested[1].tv_usec) {
        close(fd);
        return fail("timeout-getter-send");
    }
    const socklen_t short_lens[] = { 0, sizeof(int), sizeof(observed) + sizeof(int) };
    const socklen_t returned_lens[] = { 0, sizeof(int), sizeof(observed) };
    for (unsigned i = 0; i < sizeof(short_lens) / sizeof(short_lens[0]); ++i) {
        unsigned char truncated[sizeof(observed) + sizeof(int)] = {0};
        len = short_lens[i];
        if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, truncated, &len) ||
            len != returned_lens[i]) {
            close(fd);
            return fail("timeout-getter-truncated-length");
        }
        if (len >= sizeof(int)) {
            int32_t sec;
            memcpy(&sec, truncated, sizeof(sec));
            if (sec != requested[0].tv_sec) {
                close(fd);
                return fail("timeout-getter-truncated-value");
            }
        }
    }
    struct timeval negative_seconds = { .tv_sec = -1, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &negative_seconds,
                   sizeof(negative_seconds))) {
        close(fd);
        return fail("timeout-getter-negative-seconds");
    }
    len = sizeof(observed);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &observed, &len) ||
        observed.tv_sec != 0 || observed.tv_usec != 0) {
        close(fd);
        return fail("timeout-getter-negative-seconds-clamp");
    }
    struct timeval invalid = { .tv_sec = 0, .tv_usec = 1000000 };
    errno = 0;
    int rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &invalid, sizeof(invalid));
    int err = errno;
    close(fd);
    if (rc != -1 || err != EDOM) {
        errno = err;
        return fail("timeout-getter-invalid-usec");
    }
    puts("SOCKET_STRESS: socket-timeout-getters PASS");
    return 0;
}

static int recvmmsg_partial_and_timeout_restore(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return fail("recvmmsg-socketpair");
    if (send(sv[0], "abcdefgh", 8, 0) != 8) {
        close(sv[0]); close(sv[1]);
        return fail("recvmmsg-send");
    }
    char first[3] = {0}, second[5] = {0};
    struct iovec iov[2] = {
        { .iov_base = first, .iov_len = sizeof(first) },
        { .iov_base = second, .iov_len = sizeof(second) },
    };
    struct mmsghdr msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].msg_hdr.msg_iov = &iov[0];
    msgs[0].msg_hdr.msg_iovlen = 1;
    msgs[1].msg_hdr.msg_iov = &iov[1];
    msgs[1].msg_hdr.msg_iovlen = 1;
    struct timespec timeout = { .tv_sec = 5, .tv_nsec = 0 };
    struct timeval original_timeout = { .tv_sec = 0, .tv_usec = 100000 };
    if (setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO,
                   &original_timeout, sizeof(original_timeout))) {
        close(sv[0]); close(sv[1]);
        return fail("recvmmsg-set-original-timeout");
    }
    int n = recvmmsg(sv[1], msgs, 2, MSG_WAITFORONE, &timeout);
    if (n != 2 || msgs[0].msg_len != 3 || msgs[1].msg_len != 5 ||
        memcmp(first, "abc", 3) || memcmp(second, "defgh", 5)) {
        close(sv[0]); close(sv[1]);
        return fail("recvmmsg-partial-progress");
    }
    if (recvmmsg_socket_timeout_preserved(sv[1], sv[0],
                                          "recvmmsg-timeout-overwritten")) {
        close(sv[0]); close(sv[1]);
        return 1;
    }
    struct timespec failure_timeout = { .tv_sec = 5, .tv_nsec = 0 };
    errno = 0;
    if (syscall(SYS_recvmmsg, sv[1], (struct mmsghdr *)(uintptr_t)1,
                1, 0, &failure_timeout) != -1 || errno != EFAULT) {
        close(sv[0]); close(sv[1]);
        return fail("recvmmsg-error-path");
    }
    if (recvmmsg_socket_timeout_preserved(sv[1], sv[0],
                                          "recvmmsg-error-timeout-overwritten")) {
        close(sv[0]); close(sv[1]);
        return 1;
    }
    close(sv[0]); close(sv[1]);
    puts("SOCKET_STRESS: recvmmsg-partial-timeout-preserved PASS");
    return 0;
}

typedef struct {
    int fd;
    int result;
} mmsg_delayed_sender_t;

static void *mmsg_delayed_sender(void *opaque)
{
    mmsg_delayed_sender_t *arg = opaque;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 100000000L };
    nanosleep(&pause, NULL);
    arg->result = send(arg->fd, "b", 1, 0) == 1 ? 0 : -1;
    return NULL;
}

/* Without MSG_WAITFORONE recvmmsg keeps waiting for the rest of vlen up to
 * the per-call timeout. A zero-length datagram is still one consumed entry,
 * and must not terminate progression to the following datagram. */
static int mmsg_waitforone_and_zero_progress(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv))
        return fail("mmsg-waitforone-socketpair");
    struct mmsghdr msgs[2];
    struct iovec iov[2];
    char data[2] = {0};
    memset(msgs, 0, sizeof(msgs));
    iov[0] = (struct iovec){ .iov_base = &data[0], .iov_len = 1 };
    iov[1] = (struct iovec){ .iov_base = &data[1], .iov_len = 1 };
    for (int i = 0; i < 2; ++i) {
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(sv[0], "a", 1, 0) != 1) {
        close(sv[0]); close(sv[1]);
        return fail("mmsg-waitforone-first-send");
    }
    mmsg_delayed_sender_t sender = { .fd = sv[0], .result = -1 };
    pthread_t thread;
    if (pthread_create(&thread, NULL, mmsg_delayed_sender, &sender)) {
        close(sv[0]); close(sv[1]);
        return fail("mmsg-waitforone-thread");
    }
    struct timespec timeout = { .tv_sec = 2, .tv_nsec = 0 };
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    errno = 0;
    int received = recvmmsg(sv[1], msgs, 2, 0, &timeout);
    int call_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &after);
    pthread_join(thread, NULL);
    int64_t elapsed_ms = (int64_t)(after.tv_sec - before.tv_sec) * 1000 +
        (after.tv_nsec - before.tv_nsec) / 1000000;
    if (received != 2 || sender.result != 0 || call_errno != 0 ||
        elapsed_ms < 50 || data[0] != 'a' || data[1] != 'b') {
        close(sv[0]); close(sv[1]);
        return fail("mmsg-no-waitforone-blocking");
    }
    close(sv[0]); close(sv[1]);

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv))
        return fail("mmsg-zero-datagram-socketpair");
    memset(msgs, 0, sizeof(msgs));
    data[0] = data[1] = 0;
    iov[0] = (struct iovec){ .iov_base = &data[0], .iov_len = 0 };
    iov[1] = (struct iovec){ .iov_base = &data[1], .iov_len = 1 };
    for (int i = 0; i < 2; ++i) {
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(sv[0], "", 0, 0) != 0 || send(sv[0], "q", 1, 0) != 1) {
        close(sv[0]); close(sv[1]);
        return fail("mmsg-zero-datagram-seed");
    }
    errno = 0;
    received = recvmmsg(sv[1], msgs, 2, MSG_DONTWAIT, NULL);
    call_errno = errno;
    close(sv[0]); close(sv[1]);
    if (received != 2 || msgs[0].msg_len != 0 || msgs[1].msg_len != 1 ||
        data[1] != 'q' || call_errno != 0)
        return fail("mmsg-zero-datagram-progress");

    puts("SOCKET_STRESS: mmsg-waitforone-zero-progress PASS");
    return 0;
}

static void mmsg_init_pair(struct mmsghdr *vec, struct iovec *iov,
                           char payload[2])
{
    memset(vec, 0, 2 * sizeof(*vec));
    for (int i = 0; i < 2; ++i) {
        iov[i].iov_base = &payload[i];
        iov[i].iov_len = 1;
        vec[i].msg_hdr.msg_iov = &iov[i];
        vec[i].msg_hdr.msg_iovlen = 1;
    }
}

static int drain_dgrams(int fd)
{
    int count = 0;
    char byte;
    while (recv(fd, &byte, sizeof(byte), MSG_DONTWAIT) >= 0)
        count++;
    return count;
}

/* The raw syscall is intentional: musl's recvmmsg wrapper initializes each
 * msg_len before entering the kernel, which would fault on these read-only
 * output pages and skip the kernel transaction entirely. Linux executes the
 * socket operation before msg_len copyout, so the current message can have a
 * side effect even though only earlier entries count as completed. */
static int mmsg_msg_len_copyout_boundaries(void)
{
    int sv[2] = {-1, -1};
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || socketpair(AF_UNIX, SOCK_DGRAM, 0, sv))
        return fail("mmsg-copyout-setup");
    void *mapping = mmap(NULL, (size_t)page * 2,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        close(sv[0]); close(sv[1]);
        return fail("mmsg-copyout-mmap");
    }
    struct mmsghdr *vec = (struct mmsghdr *)((char *)mapping + page - sizeof(*vec));
    struct iovec iov[2];
    char payload[2] = {'a', 'b'};
    char byte;
    mmsg_init_pair(vec, iov, payload);

    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ))
        goto setup_fail;
    errno = 0;
    long sent = syscall(SYS_sendmmsg, sv[0], vec, 2, 0);
    int call_errno = errno;
    int sent_side_effects = drain_dgrams(sv[1]);
    if (sent != 1 || sent_side_effects != 2 || vec[0].msg_len != 1) {
        errno = call_errno;
        goto send_partial_fail;
    }

    if (mprotect(mapping, (size_t)page * 2, PROT_READ | PROT_WRITE))
        goto setup_fail;
    mmsg_init_pair(vec, iov, payload);
    if (mprotect(mapping, (size_t)page, PROT_READ))
        goto setup_fail;
    errno = 0;
    sent = syscall(SYS_sendmmsg, sv[0], vec, 1, 0);
    call_errno = errno;
    sent_side_effects = drain_dgrams(sv[1]);
    if (sent != -1 || call_errno != EFAULT || sent_side_effects != 1) {
        errno = call_errno;
        goto send_first_fail;
    }

    if (mprotect(mapping, (size_t)page * 2, PROT_READ | PROT_WRITE))
        goto setup_fail;
    mmsg_init_pair(vec, iov, payload);
    if (send(sv[0], "x", 1, 0) != 1 || send(sv[0], "y", 1, 0) != 1)
        goto recv_seed_fail;
    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ))
        goto setup_fail;
    errno = 0;
    long recvd = syscall(SYS_recvmmsg, sv[1], vec, 2, MSG_DONTWAIT, NULL);
    call_errno = errno;
    errno = 0;
    int remaining = recv(sv[1], &byte, 1, MSG_DONTWAIT);
    int remaining_errno = errno;
    if (recvd != 1 || vec[0].msg_len != 1 || remaining != -1 ||
        (remaining_errno != EAGAIN && remaining_errno != EWOULDBLOCK)) {
        errno = call_errno;
        goto recv_partial_fail;
    }

    if (mprotect(mapping, (size_t)page * 2, PROT_READ | PROT_WRITE))
        goto setup_fail;
    mmsg_init_pair(vec, iov, payload);
    if (send(sv[0], "z", 1, 0) != 1)
        goto recv_seed_fail;
    if (mprotect(mapping, (size_t)page, PROT_READ))
        goto setup_fail;
    errno = 0;
    recvd = syscall(SYS_recvmmsg, sv[1], vec, 1, MSG_DONTWAIT, NULL);
    call_errno = errno;
    errno = 0;
    remaining = recv(sv[1], &byte, 1, MSG_DONTWAIT);
    remaining_errno = errno;
    if (recvd != -1 || call_errno != EFAULT || remaining != -1 ||
        (remaining_errno != EAGAIN && remaining_errno != EWOULDBLOCK)) {
        errno = call_errno;
        goto recv_first_fail;
    }

    if (mprotect(mapping, (size_t)page * 2, PROT_READ | PROT_WRITE))
        goto setup_fail;
    mmsg_init_pair(vec, iov, payload);
    if (send(sv[0], "t", 1, 0) != 1)
        goto recv_seed_fail;
    struct timespec *timeout_ro = mmap(NULL, (size_t)page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (timeout_ro == MAP_FAILED)
        goto setup_fail;
    timeout_ro->tv_sec = 5;
    timeout_ro->tv_nsec = 0;
    if (mprotect(timeout_ro, (size_t)page, PROT_READ)) {
        munmap(timeout_ro, (size_t)page);
        goto setup_fail;
    }
    errno = 0;
    recvd = syscall(SYS_recvmmsg, sv[1], vec, 1, MSG_DONTWAIT, timeout_ro);
    call_errno = errno;
    errno = 0;
    remaining = recv(sv[1], &byte, 1, MSG_DONTWAIT);
    remaining_errno = errno;
    munmap(timeout_ro, (size_t)page);
    if (recvd != -1 || call_errno != EFAULT || vec[0].msg_len != 1 ||
        remaining != -1 ||
        (remaining_errno != EAGAIN && remaining_errno != EWOULDBLOCK)) {
        errno = call_errno;
        goto recv_timeout_copyout_fail;
    }

    munmap(mapping, (size_t)page * 2);
    close(sv[0]); close(sv[1]);
    puts("SOCKET_STRESS: mmsg-msg-len-copyout-side-effects PASS");
    return 0;

setup_fail:
    fail("mmsg-copyout-protection");
    goto cleanup;
send_partial_fail:
    fail("mmsg-send-partial-copyout");
    goto cleanup;
send_first_fail:
    fail("mmsg-send-first-copyout");
    goto cleanup;
recv_seed_fail:
    fail("mmsg-recv-seed");
    goto cleanup;
recv_partial_fail:
    fail("mmsg-recv-partial-copyout");
    goto cleanup;
recv_first_fail:
    fail("mmsg-recv-first-copyout");
    goto cleanup;
recv_timeout_copyout_fail:
    fail("mmsg-timeout-copyout-side-effect");
cleanup:
    munmap(mapping, (size_t)page * 2);
    close(sv[0]); close(sv[1]);
    return 1;
}

static int mmsg_vlen_clamp_contract(void)
{
    int sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv))
        return fail("mmsg-vlen-socketpair");
    errno = 0;
    long rc = syscall(SYS_sendmmsg, sv[0], (void *)(uintptr_t)1, 1025, 0);
    int err = errno;
    if (rc != -1 || err != EFAULT) {
        close(sv[0]); close(sv[1]); errno = err;
        return fail("mmsg-vlen-send-clamp");
    }
    errno = 0;
    rc = syscall(SYS_recvmmsg, sv[0], (void *)(uintptr_t)1, 1025,
                 MSG_DONTWAIT, NULL);
    err = errno;
    if (rc != -1 || err != EFAULT) {
        close(sv[0]); close(sv[1]); errno = err;
        return fail("mmsg-vlen-recv-clamp");
    }
    errno = 0;
    rc = syscall(SYS_sendmmsg, -1, (void *)(uintptr_t)1, 1025, 0);
    err = errno;
    if (rc != -1 || err != EBADF) {
        close(sv[0]); close(sv[1]); errno = err;
        return fail("mmsg-vlen-send-fd-precedence");
    }
    errno = 0;
    rc = syscall(SYS_recvmmsg, -1, (void *)(uintptr_t)1, 1025,
                 MSG_DONTWAIT, NULL);
    err = errno;
    close(sv[0]); close(sv[1]);
    if (rc != -1 || err != EBADF) {
        errno = err;
        return fail("mmsg-vlen-recv-fd-precedence");
    }
    puts("SOCKET_STRESS: mmsg-vlen-clamp-contract PASS");
    return 0;
}

int main(void)
{
    printf("SOCKET_STRESS: start\n");
    /* Each registering CPU starts in a different registry shard. Repeated
     * close/reopen must recycle that shard's local bits, without touching a
     * different shard or exhausting the global table. */
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available))
        return fail("registry-affinity");
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &available)) continue;
        int start[2], ready[2];
        if (pipe(start) || pipe(ready)) return fail("registry-pipe");
        pid_t child = fork();
        if (!child) {
            close(start[1]); close(ready[0]);
            char c;
            if (write(ready[1], "r", 1) != 1 || read(start[0], &c, 1) != 1)
                _exit(1);
            close(start[0]); close(ready[1]);
            for (int i = 0; i < 1100; i++) {
                int pair[2];
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
                    fprintf(stderr, "SOCKET_STRESS: cpu=%d iteration=%d errno=%d\n", cpu, i, errno);
                    _exit(2);
                }
                close(pair[0]); close(pair[1]);
            }
            _exit(0);
        }
        close(start[0]); close(ready[1]);
        char c;
        int ok = child > 0 && read(ready[0], &c, 1) == 1;
        cpu_set_t one;
        CPU_ZERO(&one); CPU_SET(cpu, &one);
        /* Configure the sleeping worker before it resumes. The readiness
         * byte can reach us before its read has finished entering sleep. */
        if (ok) {
            for (int retry = 0; retry < 100; retry++) {
                if (!sched_setaffinity(child, sizeof(one), &one)) break;
                if (retry == 99) ok = 0;
                usleep(1000);
            }
        }
        if (ok) ok = write(start[1], "s", 1) == 1;
        close(start[1]); close(ready[0]);
        int status;
        pid_t waited = child > 0 ? waitpid(child, &status, 0) : -1;
        if (!ok || waited != child ||
            !WIFEXITED(status) || WEXITSTATUS(status))
            return fail("registry-shard-recycle");
    }
    puts("SOCKET_STRESS: registry-shard-recycle PASS");
    if (socket_queue_accounting() != 0)
        return 1;
    if (socket_close_reuse_pinned_recv(0) != 0)
        return 1;
    if (socket_close_reuse_pinned_recv(1) != 0)
        return 1;
    if (recvmmsg_partial_and_timeout_restore() != 0)
        return 1;
    if (socket_timeout_getter_roundtrip() != 0)
        return 1;
    if (mmsg_msg_len_copyout_boundaries() != 0)
        return 1;
    if (mmsg_vlen_clamp_contract() != 0)
        return 1;
    if (mmsg_waitforone_and_zero_progress() != 0)
        return 1;
    if (concurrent_tcp_stress() != 0)
        return 1;
    if (udp_ping_pong() != 0)
        return 1;
    printf("SOCKET_STRESS: PASS\n");
    return 0;
}
