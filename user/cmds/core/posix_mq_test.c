#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHK(cond, text)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("POSIX_MQ_TEST: FAIL %s:%d %s errno=%d (%s)\n",          \
                   __func__, __LINE__, text, errno, strerror(errno));           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct payload {
    uint32_t first;
    uint32_t second;
};

static volatile sig_atomic_t notifications;

static void notification_handler(int signo)
{
    (void)signo;
    notifications++;
}

static int wait_child(pid_t child)
{
    int status = 0;
    CHK(waitpid(child, &status, 0) == child, "waitpid");
    CHK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child status");
    return 0;
}

static mqd_t open_queue(const char *name, long maxmsg, int nonblock)
{
    mq_unlink(name);
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = maxmsg;
    attr.mq_msgsize = 16;
    return mq_open(name, O_CREAT | O_EXCL | O_RDWR |
                   (nonblock ? O_NONBLOCK : 0), 0600, &attr);
}

static int test_order_and_errors(void)
{
    mqd_t mq = open_queue("/a20_mq_order", 4, 1);
    CHK(mq >= 0, "mq_open order");
    puts("POSIX_MQ_TEST: open PASS");
    struct payload message;
    memset(&message, 0, sizeof(message));
    const unsigned priorities[4] = {1, 3, 1, 0};
    for (unsigned i = 0; i < 4; i++) {
        message.first = i;
        CHK(mq_send(mq, (const char *)&message, sizeof(message),
                    priorities[i]) == 0, "mq_send order");
    }
    puts("POSIX_MQ_TEST: initial sends PASS");

    errno = 0;
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 32) == -1 &&
        errno == EINVAL, "invalid priority");
    char oversized[17] = {0};
    errno = 0;
    CHK(mq_send(mq, oversized, sizeof(oversized), 0) == -1 &&
        errno == EMSGSIZE, "oversized message");
    errno = 0;
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 0) == -1 &&
        errno == EAGAIN, "full nonblocking queue");

    struct mq_attr attr;
    CHK(mq_getattr(mq, &attr) == 0 && attr.mq_curmsgs == 4,
        "queue count after rejected sends");
    char small[1];
    errno = 0;
    CHK(mq_receive(mq, small, sizeof(small), NULL) == -1 &&
        errno == EMSGSIZE, "small receive buffer");
    CHK(mq_getattr(mq, &attr) == 0 && attr.mq_curmsgs == 4,
        "small buffer must not dequeue");

    const unsigned expected[4] = {1, 0, 2, 3};
    for (unsigned i = 0; i < 4; i++) {
        struct payload received;
        unsigned prio = 99;
        CHK(mq_receive(mq, (char *)&received, sizeof(received), &prio) ==
            (ssize_t)sizeof(received), "mq_receive order");
        CHK(received.first == expected[i] && prio == priorities[expected[i]],
            "priority or FIFO order");
    }
    puts("POSIX_MQ_TEST: priority receives PASS");
    errno = 0;
    CHK(mq_receive(mq, (char *)&message, sizeof(message), NULL) == -1 &&
        errno == EAGAIN, "empty nonblocking queue");
    CHK(mq_close(mq) == 0, "mq_close order");
    puts("POSIX_MQ_TEST: close PASS");
    errno = 0;
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 0) == -1 &&
        errno == EBADF, "closed descriptor");
    CHK(mq_unlink("/a20_mq_order") == 0, "mq_unlink order");
    return 0;
}

static int test_blocking_and_timeout(void)
{
    mqd_t mq = open_queue("/a20_mq_blocking", 1, 0);
    CHK(mq >= 0, "mq_open blocking");
    struct payload message = {.first = 11, .second = 12};
    pid_t child = fork();
    CHK(child >= 0, "fork receive waiter");
    if (child == 0) {
        alarm(3);
        usleep(50000);
        _exit(mq_send(mq, (const char *)&message, sizeof(message), 7) == 0 ? 0 : 2);
    }
    struct payload received;
    unsigned prio = 0;
    CHK(mq_receive(mq, (char *)&received, sizeof(received), &prio) ==
        (ssize_t)sizeof(received) && received.first == 11 && prio == 7,
        "blocked receive");
    CHK(wait_child(child) == 0, "receive sender");
    puts("POSIX_MQ_TEST: blocked receive PASS");

    CHK(mq_send(mq, (const char *)&message, sizeof(message), 1) == 0,
        "fill blocking queue");
    child = fork();
    CHK(child >= 0, "fork send waiter");
    if (child == 0) {
        alarm(3);
        usleep(50000);
        _exit(mq_receive(mq, (char *)&received, sizeof(received), NULL) ==
              (ssize_t)sizeof(received) ? 0 : 3);
    }
    message.first = 22;
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 2) == 0,
        "blocked send");
    CHK(wait_child(child) == 0, "send receiver");
    puts("POSIX_MQ_TEST: blocked send PASS");
    CHK(mq_receive(mq, (char *)&received, sizeof(received), &prio) ==
        (ssize_t)sizeof(received) && received.first == 22 && prio == 2,
        "message after blocked send");

    struct timespec deadline;
    CHK(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "clock_gettime");
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    errno = 0;
    CHK(mq_timedreceive(mq, (char *)&received, sizeof(received), NULL,
                        &deadline) == -1 && errno == ETIMEDOUT,
        "timed empty receive");
    puts("POSIX_MQ_TEST: timed receive PASS");
    CHK(mq_close(mq) == 0 && mq_unlink("/a20_mq_blocking") == 0,
        "blocking cleanup");
    return 0;
}

static int test_notification(void)
{
    mqd_t mq = open_queue("/a20_mq_notify", 2, 1);
    CHK(mq >= 0, "mq_open notify");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = notification_handler;
    sigemptyset(&action.sa_mask);
    CHK(sigaction(SIGUSR1, &action, NULL) == 0, "sigaction");
    struct sigevent event;
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_SIGNAL;
    event.sigev_signo = SIGUSR1;
    CHK(mq_notify(mq, &event) == 0, "mq_notify register");
    struct payload message = {.first = 33, .second = 44};
    notifications = 0;
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 0) == 0,
        "mq_send notify");
    for (int i = 0; i < 100 && !notifications; i++)
        usleep(1000);
    CHK(notifications == 1, "notification signal");
    CHK(mq_send(mq, (const char *)&message, sizeof(message), 0) == 0,
        "second mq_send notify");
    usleep(10000);
    CHK(notifications == 1, "notification must be one-shot");
    CHK(mq_notify(mq, NULL) == 0, "mq_notify unregister");
    CHK(mq_close(mq) == 0 && mq_unlink("/a20_mq_notify") == 0,
        "notify cleanup");
    return 0;
}

static int test_pressure(void)
{
    enum {PRODUCERS = 4, EACH = 32, TOTAL = PRODUCERS * EACH};
    mqd_t mq = open_queue("/a20_mq_pressure", 4, 0);
    CHK(mq >= 0, "mq_open pressure");
    pid_t children[PRODUCERS];
    for (uint32_t producer = 0; producer < PRODUCERS; producer++) {
        pid_t child = fork();
        CHK(child >= 0, "fork producer");
        if (child == 0) {
            alarm(10);
            for (uint32_t sequence = 0; sequence < EACH; sequence++) {
                struct payload message = {producer, sequence};
                if (mq_send(mq, (const char *)&message, sizeof(message), 0) != 0)
                    _exit(10);
            }
            _exit(0);
        }
        children[producer] = child;
    }

    unsigned seen[PRODUCERS][EACH];
    memset(seen, 0, sizeof(seen));
    alarm(10);
    for (unsigned i = 0; i < TOTAL; i++) {
        struct payload received;
        CHK(mq_receive(mq, (char *)&received, sizeof(received), NULL) ==
            (ssize_t)sizeof(received), "pressure receive");
        CHK(received.first < PRODUCERS && received.second < EACH,
            "pressure payload range");
        CHK(!seen[received.first][received.second], "duplicate payload");
        seen[received.first][received.second] = 1;
    }
    alarm(0);
    for (unsigned i = 0; i < PRODUCERS; i++)
        CHK(wait_child(children[i]) == 0, "producer status");
    for (unsigned producer = 0; producer < PRODUCERS; producer++)
        for (unsigned sequence = 0; sequence < EACH; sequence++)
            CHK(seen[producer][sequence], "missing payload");
    CHK(mq_close(mq) == 0 && mq_unlink("/a20_mq_pressure") == 0,
        "pressure cleanup");
    puts("POSIX_MQ_TEST: pressure senders=4 messages=128 PASS");
    return 0;
}

static int bench_roundtrip(void)
{
    enum {ITERATIONS = 512};
    mqd_t tx = open_queue("/a20_mq_bench_tx", 8, 0);
    CHK(tx >= 0, "mq_open bench tx");
    mqd_t rx = open_queue("/a20_mq_bench_rx", 8, 0);
    CHK(rx >= 0, "mq_open bench rx");
    pid_t child = fork();
    CHK(child >= 0, "fork bench");
    if (child == 0) {
        alarm(20);
        for (uint32_t i = 0; i < ITERATIONS; i++) {
            struct payload message;
            if (mq_receive(tx, (char *)&message, sizeof(message), NULL) !=
                (ssize_t)sizeof(message))
                _exit(20);
            if (mq_send(rx, (const char *)&message, sizeof(message), 0) != 0)
                _exit(21);
        }
        _exit(0);
    }

    struct timespec start, end;
    CHK(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "bench start");
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        struct payload message = {i, ~i};
        CHK(mq_send(tx, (const char *)&message, sizeof(message), 0) == 0,
            "bench send");
        CHK(mq_receive(rx, (char *)&message, sizeof(message), NULL) ==
            (ssize_t)sizeof(message) && message.first == i && message.second == ~i,
            "bench receive");
    }
    CHK(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "bench end");
    CHK(wait_child(child) == 0, "bench child");
    uint64_t elapsed = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ULL +
                       (uint64_t)(end.tv_nsec - start.tv_nsec);
    CHK(elapsed > 0, "bench elapsed");
    printf("POSIX_MQ_BENCH: iterations=%u elapsed_ns=%llu throughput=%llu/s round_trip_ns=%llu\n",
           ITERATIONS, (unsigned long long)elapsed,
           (unsigned long long)(ITERATIONS * 1000000000ULL / elapsed),
           (unsigned long long)(elapsed / ITERATIONS));
    CHK(mq_close(tx) == 0 && mq_close(rx) == 0, "bench close");
    CHK(mq_unlink("/a20_mq_bench_tx") == 0 &&
        mq_unlink("/a20_mq_bench_rx") == 0, "bench unlink");
    puts("POSIX_MQ_BENCH: PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--bench") == 0)
        return bench_roundtrip();
    if (test_order_and_errors()) {
        puts("POSIX_MQ_TEST: FAIL");
        return 1;
    }
    puts("POSIX_MQ_TEST: order/errors PASS");
    if (test_blocking_and_timeout()) {
        puts("POSIX_MQ_TEST: FAIL");
        return 1;
    }
    if (test_notification()) {
        puts("POSIX_MQ_TEST: FAIL");
        return 1;
    }
    puts("POSIX_MQ_TEST: notification PASS");
    if (test_pressure()) {
        puts("POSIX_MQ_TEST: FAIL");
        return 1;
    }
    puts("POSIX_MQ_TEST: PASS");
    return 0;
}
