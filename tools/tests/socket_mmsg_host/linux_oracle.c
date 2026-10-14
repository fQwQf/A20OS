#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static int fail(const char *case_name) {
    fprintf(stderr, "oracle assertion failed: %s\n", case_name);
    return 1;
}

struct delayed_send {
    int fd;
    long delay_ns;
};

struct timespec32_wire {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct timespec32_canary {
    uint32_t before;
    struct timespec32_wire value;
    uint32_t after;
};

_Static_assert(sizeof(struct timespec32_wire) == 8,
               "32-bit recvmmsg timespec must occupy exactly 8 bytes");
_Static_assert(offsetof(struct timespec32_wire, tv_nsec) == 4,
               "32-bit recvmmsg nsec offset");

static void *send_later(void *opaque) {
    struct delayed_send *args = opaque;
    struct timespec pause = { .tv_sec = 0, .tv_nsec = args->delay_ns };
    nanosleep(&pause, NULL);
    (void)send(args->fd, "b", 1, 0);
    return NULL;
}

static int drain(int fd) {
    int n = 0;
    char b[32];
    while (recv(fd, b, sizeof(b), MSG_DONTWAIT) >= 0) ++n;
    return n;
}

int main(void) {
    struct timespec32_canary canary = {
        .before = 0x12345678,
        .value = { .tv_sec = 0x10203040, .tv_nsec = 0x50607080 },
        .after = 0x89abcdef,
    };
    struct timespec32_wire copied32;
    memcpy(&copied32, (const unsigned char *)&canary +
           offsetof(struct timespec32_canary, value), sizeof(copied32));
    printf("timespec32_layout bytes=%zu nsec_offset=%zu guards=%08x/%08x\n",
           sizeof(copied32), offsetof(struct timespec32_wire, tv_nsec),
           canary.before, canary.after);
    if (copied32.tv_sec != 0x10203040 || copied32.tv_nsec != 0x50607080 ||
        canary.before != 0x12345678 || canary.after != 0x89abcdef)
        return fail("32-bit timespec buffer canary");

    long page = sysconf(_SC_PAGESIZE);
    void *mapping = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) { perror("mmap"); return 2; }
    struct mmsghdr *vec = (struct mmsghdr *)((char *)mapping + page - sizeof(*vec));
    struct iovec iov[2];
    char payloads[2] = {'a', 'b'};
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv)) { perror("socketpair"); return 2; }
    memset(vec, 0, 2 * sizeof(*vec));
    memset(iov, 0, sizeof(iov));
    for (int i = 0; i < 2; ++i) {
        iov[i].iov_base = &payloads[i]; iov[i].iov_len = 1;
        vec[i].msg_hdr.msg_iov = &iov[i]; vec[i].msg_hdr.msg_iovlen = 1;
    }
    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ)) { perror("mprotect"); return 2; }
    errno = 0;
    long sent = syscall(SYS_sendmmsg, sv[0], vec, 2, 0);
    int sent_errno = errno;
    int sent_side_effects = drain(sv[1]);
    printf("send_partial_copyout ret=%ld errno=%d side_effects=%d first_len=%u\n",
           sent, sent_errno, sent_side_effects, vec[0].msg_len);
    if (sent != 1 || sent_errno != 0 || sent_side_effects != 2 ||
        vec[0].msg_len != 1)
        return fail("send partial msg_len copyout");

    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 2; }
    memset(vec, 0, 2 * sizeof(*vec));
    for (int i = 0; i < 2; ++i) {
        iov[i].iov_base = &payloads[i]; iov[i].iov_len = 1;
        vec[i].msg_hdr.msg_iov = &iov[i]; vec[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(sv[0], "x", 1, 0) != 1 || send(sv[0], "y", 1, 0) != 1) { perror("send seed"); return 2; }
    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ)) { perror("mprotect"); return 2; }
    errno = 0;
    long recvd = syscall(SYS_recvmmsg, sv[1], vec, 2, MSG_DONTWAIT, NULL);
    int recv_errno = errno;
    char check;
    int remaining = recv(sv[1], &check, 1, MSG_DONTWAIT);
    int recv_remaining = remaining >= 0;
    printf("recv_partial_copyout ret=%ld errno=%d remaining=%d first_len=%u\n",
           recvd, recv_errno, recv_remaining, vec[0].msg_len);
    if (recvd != 1 || recv_errno != 0 || recv_remaining != 0 ||
        vec[0].msg_len != 1)
        return fail("recv partial msg_len copyout");

    if (mprotect((char *)mapping + page, (size_t)page, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 2; }
    memset(vec, 0, 2 * sizeof(*vec));
    for (int i = 0; i < 2; ++i) {
        iov[i].iov_base = &payloads[i]; iov[i].iov_len = 1;
        vec[i].msg_hdr.msg_iov = &iov[i]; vec[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(sv[0], "z", 1, 0) != 1) { perror("send seed2"); return 2; }
    if (mprotect(mapping, (size_t)page, PROT_READ)) { perror("mprotect first"); return 2; }
    errno = 0;
    long first_recv = syscall(SYS_recvmmsg, sv[1], vec, 1, MSG_DONTWAIT, NULL);
    int first_errno = errno;
    remaining = recv(sv[1], &check, 1, MSG_DONTWAIT);
    printf("recv_first_copyout ret=%ld errno=%d consumed=%d\n",
           first_recv, first_errno, remaining < 0 && errno == EAGAIN);
    if (first_recv != -1 || first_errno != EFAULT || remaining >= 0 ||
        errno != EAGAIN)
        return fail("recv first msg_len copyout");

    struct mmsghdr one = {0};
    char out;
    struct iovec one_iov = { .iov_base = &out, .iov_len = sizeof(out) };
    one.msg_hdr.msg_iov = &one_iov;
    one.msg_hdr.msg_iovlen = 1;
    struct timespec timeout = { .tv_sec = 5, .tv_nsec = 0 };
    if (send(sv[0], "t", 1, 0) != 1) { perror("send timeout seed"); return 2; }
    errno = 0;
    long timeout_ok = syscall(SYS_recvmmsg, sv[1], &one, 1,
                              MSG_DONTWAIT, &timeout);
    printf("recv_timeout_update ret=%ld errno=%d remaining=%ld.%09ld\n",
           timeout_ok, errno, (long)timeout.tv_sec, timeout.tv_nsec);
    if (timeout_ok != 1 || timeout.tv_sec < 0 || timeout.tv_sec > 5 ||
        timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000L)
        return fail("recvmmsg timeout writeback");

    struct timespec *timeout_ro = mmap(NULL, (size_t)page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (timeout_ro == MAP_FAILED) { perror("timeout mmap"); return 2; }
    *timeout_ro = (struct timespec){ .tv_sec = 5, .tv_nsec = 0 };
    if (mprotect(timeout_ro, (size_t)page, PROT_READ)) { perror("timeout mprotect"); return 2; }
    if (send(sv[0], "u", 1, 0) != 1) { perror("send timeout ro seed"); return 2; }
    errno = 0;
    long timeout_fault = syscall(SYS_recvmmsg, sv[1], &one, 1,
                                 MSG_DONTWAIT, timeout_ro);
    int timeout_fault_errno = errno;
    errno = 0;
    remaining = recv(sv[1], &check, 1, MSG_DONTWAIT);
    int timeout_consumed = remaining < 0 && errno == EAGAIN;
    printf("recv_timeout_copyout ret=%ld errno=%d consumed=%d msg_len=%u\n",
           timeout_fault, timeout_fault_errno, timeout_consumed, one.msg_len);
    if (timeout_fault != -1 || timeout_fault_errno != EFAULT ||
        !timeout_consumed || one.msg_len != 1)
        return fail("recvmmsg timeout copyout");
    munmap(timeout_ro, (size_t)page);

    /* Without MSG_WAITFORONE, recvmmsg waits for the rest of vlen up to its
     * timeout; it must not silently switch to MSG_DONTWAIT after item one. */
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) { perror("delay socketpair"); return 2; }
    struct mmsghdr delayed_vec[2] = {0};
    char delayed_buf[2] = {0};
    struct iovec delayed_iov[2] = {
        { .iov_base = &delayed_buf[0], .iov_len = 1 },
        { .iov_base = &delayed_buf[1], .iov_len = 1 },
    };
    for (int i = 0; i < 2; ++i) {
        delayed_vec[i].msg_hdr.msg_iov = &delayed_iov[i];
        delayed_vec[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(pair[0], "a", 1, 0) != 1) { perror("delay seed"); return 2; }
    struct delayed_send delay = { .fd = pair[0], .delay_ns = 100000000L };
    pthread_t sender;
    if (pthread_create(&sender, NULL, send_later, &delay)) { perror("pthread_create"); return 2; }
    struct timespec delayed_timeout = { .tv_sec = 1, .tv_nsec = 0 };
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    errno = 0;
    long delayed_count = syscall(SYS_recvmmsg, pair[1], delayed_vec, 2, 0,
                                 &delayed_timeout);
    int delayed_errno = errno;
    clock_gettime(CLOCK_MONOTONIC, &after);
    pthread_join(sender, NULL);
    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000L +
        (after.tv_nsec - before.tv_nsec) / 1000000L;
    printf("recv_no_waitforone_delayed ret=%ld errno=%d elapsed_ms=%ld\n",
           delayed_count, delayed_errno, elapsed_ms);
    if (delayed_count != 2 || delayed_errno != 0 || elapsed_ms < 50 ||
        delayed_buf[0] != 'a' || delayed_buf[1] != 'b')
        return fail("recvmmsg no-WAITFORONE blocking");
    close(pair[0]); close(pair[1]);

    /* A zero-size datagram is a message, not stream EOF; it advances the
     * vector and permits the following queued datagram to be received. */
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) { perror("zero socketpair"); return 2; }
    struct mmsghdr zero_vec[2] = {0};
    char zero_byte = 0;
    struct iovec zero_iov[2] = {
        { .iov_base = &zero_byte, .iov_len = 0 },
        { .iov_base = &zero_byte, .iov_len = 1 },
    };
    for (int i = 0; i < 2; ++i) {
        zero_vec[i].msg_hdr.msg_iov = &zero_iov[i];
        zero_vec[i].msg_hdr.msg_iovlen = 1;
    }
    if (send(pair[0], "", 0, 0) != 0 || send(pair[0], "q", 1, 0) != 1) {
        perror("zero datagram seed"); return 2;
    }
    errno = 0;
    long zero_count = syscall(SYS_recvmmsg, pair[1], zero_vec, 2,
                              MSG_DONTWAIT, NULL);
    printf("recv_zero_datagram ret=%ld errno=%d lengths=%u,%u\n",
           zero_count, errno, zero_vec[0].msg_len, zero_vec[1].msg_len);
    if (zero_count != 2 || zero_vec[0].msg_len != 0 ||
        zero_vec[1].msg_len != 1 || zero_byte != 'q')
        return fail("recvmmsg zero datagram progress");
    close(pair[0]); close(pair[1]);

    /* EOF on streams also returns zero; native Linux advances every mmsghdr
     * entry rather than confusing it with an empty datagram. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { perror("eof socketpair"); return 2; }
    close(pair[0]);
    struct mmsghdr eof_vec[2] = {0};
    struct iovec eof_iov[2] = {
        { .iov_base = &zero_byte, .iov_len = 1 },
        { .iov_base = &zero_byte, .iov_len = 1 },
    };
    for (int i = 0; i < 2; ++i) {
        eof_vec[i].msg_hdr.msg_iov = &eof_iov[i];
        eof_vec[i].msg_hdr.msg_iovlen = 1;
    }
    errno = 0;
    long eof_count = syscall(SYS_recvmmsg, pair[1], eof_vec, 2,
                             MSG_DONTWAIT, NULL);
    printf("recv_stream_eof ret=%ld errno=%d lengths=%u,%u\n",
           eof_count, errno, eof_vec[0].msg_len, eof_vec[1].msg_len);
    if (eof_count != 2 || eof_vec[0].msg_len != 0 || eof_vec[1].msg_len != 0)
        return fail("recvmmsg stream EOF progress");
    close(pair[1]);

    int timeout_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (timeout_fd < 0) { perror("timeout socket"); return 2; }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    if (setsockopt(timeout_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
        perror("setsockopt SO_RCVTIMEO"); return 2;
    }
    const socklen_t request_lengths[] = { 0, sizeof(int), sizeof(tv), sizeof(tv) + sizeof(int) };
    const socklen_t expected_lengths[] = { 0, sizeof(int), sizeof(tv), sizeof(tv) };
    for (unsigned i = 0; i < sizeof(request_lengths) / sizeof(request_lengths[0]); ++i) {
        unsigned char out[sizeof(tv) + sizeof(int)];
        memset(out, 0xa5, sizeof(out));
        socklen_t outlen = request_lengths[i];
        errno = 0;
        long rc = syscall(SYS_getsockopt, timeout_fd, SOL_SOCKET, SO_RCVTIMEO,
                          out, &outlen);
        printf("getsockopt_timeout request=%u ret=%ld errno=%d returned=%u\n",
               request_lengths[i], rc, errno, outlen);
        if (rc != 0 || outlen != expected_lengths[i])
            return fail("getsockopt timeout truncation length");
        if (outlen >= sizeof(int)) {
            int32_t sec;
            memcpy(&sec, out, sizeof(sec));
            if (sec != 2) return fail("getsockopt timeout truncated value");
        }
        if (outlen == sizeof(tv)) {
            struct timeval got;
            memcpy(&got, out, sizeof(got));
            if (got.tv_sec != 2 || got.tv_usec != 0)
                return fail("getsockopt timeout full value");
        }
    }
    const struct timeval invalid_timeouts[] = {
        { .tv_sec = -1, .tv_usec = 0 },
        { .tv_sec = 0, .tv_usec = -1 },
        { .tv_sec = 0, .tv_usec = 1000000 },
    };
    const int expected_set_result[] = { 0, -1, -1 };
    const int expected_set_errno[] = { 0, EDOM, EDOM };
    for (unsigned i = 0; i < sizeof(invalid_timeouts) / sizeof(invalid_timeouts[0]); ++i) {
        errno = 0;
        int rc = setsockopt(timeout_fd, SOL_SOCKET, SO_RCVTIMEO,
                            &invalid_timeouts[i], sizeof(invalid_timeouts[i]));
        printf("setsockopt_timeout_invalid index=%u ret=%d errno=%d\n",
               i, rc, errno);
        if (rc != expected_set_result[i] || errno != expected_set_errno[i])
            return fail("setsockopt timeout errno/value behavior");
        struct timeval readback = {0};
        socklen_t readback_len = sizeof(readback);
        if (getsockopt(timeout_fd, SOL_SOCKET, SO_RCVTIMEO,
                       &readback, &readback_len) == 0)
            printf("setsockopt_timeout_readback index=%u sec=%ld usec=%ld\n",
                   i, (long)readback.tv_sec, (long)readback.tv_usec);
        if (i == 0 && (readback.tv_sec != 0 || readback.tv_usec != 0))
            return fail("negative timeval seconds clamp to zero");
    }
    close(timeout_fd);

    int vlen_pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, vlen_pair)) { perror("vlen socketpair"); return 2; }
    errno = 0;
    long vlen_send = syscall(SYS_sendmmsg, vlen_pair[0], (void *)(uintptr_t)1,
                             1025, 0);
    int vlen_send_errno = errno;
    errno = 0;
    long vlen_recv = syscall(SYS_recvmmsg, vlen_pair[0], (void *)(uintptr_t)1,
                             1025, MSG_DONTWAIT, NULL);
    int vlen_recv_errno = errno;
    errno = 0;
    long vlen_badfd_send = syscall(SYS_sendmmsg, -1, (void *)(uintptr_t)1,
                                   1025, 0);
    int vlen_badfd_send_errno = errno;
    errno = 0;
    long vlen_badfd_recv = syscall(SYS_recvmmsg, -1, (void *)(uintptr_t)1,
                                   1025, MSG_DONTWAIT, NULL);
    int vlen_badfd_recv_errno = errno;
    printf("mmsg_vlen_over_limit send=%ld/%d recv=%ld/%d badfd_send=%ld/%d badfd_recv=%ld/%d\n",
           vlen_send, vlen_send_errno, vlen_recv, vlen_recv_errno,
           vlen_badfd_send, vlen_badfd_send_errno,
           vlen_badfd_recv, vlen_badfd_recv_errno);
    if (vlen_send != -1 || vlen_send_errno != EFAULT ||
        vlen_recv != -1 || vlen_recv_errno != EFAULT ||
        vlen_badfd_send != -1 || vlen_badfd_send_errno != EBADF ||
        vlen_badfd_recv != -1 || vlen_badfd_recv_errno != EBADF)
        return fail("mmsg vlen clamp and fd precedence");
    close(vlen_pair[0]); close(vlen_pair[1]);

    close(sv[0]); close(sv[1]);
    munmap(mapping, (size_t)page * 2);
    return 0;
}
