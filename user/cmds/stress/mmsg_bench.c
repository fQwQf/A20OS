#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* RV32 exposes only the time64 recvmmsg syscall number. This benchmark uses
 * a NULL timeout, so the time64 entry has the same request semantics. */
#if !defined(SYS_recvmmsg) && defined(SYS_recvmmsg_time64)
#define SYS_recvmmsg SYS_recvmmsg_time64
#endif

#define BATCH_MAX 8
#define LOOPS 5000
#define PAYLOAD_BYTES 64

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int validate_batch(const struct mmsghdr *send_vec,
                          const struct mmsghdr *recv_vec,
                          const unsigned char payload[BATCH_MAX][PAYLOAD_BYTES],
                          const unsigned char received[BATCH_MAX][PAYLOAD_BYTES],
                          unsigned batch)
{
    for (unsigned i = 0; i < batch; ++i) {
        if (send_vec[i].msg_len != PAYLOAD_BYTES ||
            recv_vec[i].msg_len != PAYLOAD_BYTES ||
            memcmp(payload[i], received[i], PAYLOAD_BYTES) != 0)
            return -1;
    }
    return 0;
}

static int run_batch(int fd[2], unsigned batch)
{
    struct mmsghdr send_vec[BATCH_MAX];
    struct mmsghdr recv_vec[BATCH_MAX];
    struct iovec send_iov[BATCH_MAX];
    struct iovec recv_iov[BATCH_MAX];
    unsigned char payload[BATCH_MAX][PAYLOAD_BYTES];
    unsigned char received[BATCH_MAX][PAYLOAD_BYTES];
    memset(send_vec, 0, sizeof(send_vec));
    memset(recv_vec, 0, sizeof(recv_vec));
    memset(payload, 0x5a, sizeof(payload));
    memset(received, 0, sizeof(received));
    for (unsigned i = 0; i < batch; ++i) {
        send_iov[i].iov_base = payload[i];
        send_iov[i].iov_len = sizeof(payload[i]);
        send_vec[i].msg_hdr.msg_iov = &send_iov[i];
        send_vec[i].msg_hdr.msg_iovlen = 1;
        recv_iov[i].iov_base = received[i];
        recv_iov[i].iov_len = sizeof(received[i]);
        recv_vec[i].msg_hdr.msg_iov = &recv_iov[i];
        recv_vec[i].msg_hdr.msg_iovlen = 1;
    }

    /* Warm up both controller paths before timing. */
    long sent = syscall(SYS_sendmmsg, fd[0], send_vec, batch, 0);
    long got = syscall(SYS_recvmmsg, fd[1], recv_vec, batch,
                       MSG_DONTWAIT, NULL);
    if (sent != (long)batch || got != (long)batch ||
        validate_batch(send_vec, recv_vec, payload, received, batch) < 0)
        return -1;

    uint64_t start = now_ns();
    if (!start)
        return -1;
    for (unsigned n = 0; n < LOOPS; ++n) {
        sent = syscall(SYS_sendmmsg, fd[0], send_vec, batch, 0);
        if (sent != (long)batch)
            return -1;
        got = syscall(SYS_recvmmsg, fd[1], recv_vec, batch,
                      MSG_DONTWAIT, NULL);
        if (got != (long)batch)
            return -1;
    }
    uint64_t end = now_ns();
    if (end <= start ||
        validate_batch(send_vec, recv_vec, payload, received, batch) < 0)
        return -1;
    uint64_t elapsed = end - start;
    uint64_t messages = (uint64_t)LOOPS * batch;
    uint64_t msg_per_sec = messages * 1000000000ULL / elapsed;
    uint64_t ns_per_syscall = elapsed / ((uint64_t)LOOPS * 2);
    printf("MMSG_BENCH: batch=%u loops=%u elapsed_ns=%llu messages_per_sec=%llu ns_per_send_or_recv=%llu\n",
           batch, LOOPS, (unsigned long long)elapsed,
           (unsigned long long)msg_per_sec,
           (unsigned long long)ns_per_syscall);
    return 0;
}

int main(void)
{
    int fd[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fd) < 0) {
        printf("MMSG_BENCH: FAIL socketpair errno=%d\n", errno);
        return 1;
    }
    int rc1 = run_batch(fd, 1);
    int rc8 = run_batch(fd, 8);
    close(fd[0]);
    close(fd[1]);
    if (rc1 || rc8) {
        printf("MMSG_BENCH: FAIL operation errno=%d\n", errno);
        return 1;
    }
    puts("MMSG_BENCH: PASS");
    return 0;
}
