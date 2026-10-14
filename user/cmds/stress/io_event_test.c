#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define TEST_FILE "/tmp/io_event_test.txt"
#define TEST_MSG  "io_event_block_data"
#define TEST_PORT 12347

static int test_block_io(void)
{
    int fd = open(TEST_FILE, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) {
        printf("IO_EVENT_TEST: FAIL block open errno=%d\n", errno);
        return -1;
    }

    if (write(fd, TEST_MSG, strlen(TEST_MSG)) != (ssize_t)strlen(TEST_MSG)) {
        printf("IO_EVENT_TEST: FAIL block write errno=%d\n", errno);
        close(fd);
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf("IO_EVENT_TEST: FAIL block lseek errno=%d\n", errno);
        close(fd);
        return -1;
    }

    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n != (ssize_t)strlen(TEST_MSG) || strcmp(buf, TEST_MSG) != 0) {
        printf("IO_EVENT_TEST: FAIL block read mismatch n=%zd buf=%s\n", n, buf);
        close(fd);
        return -1;
    }

    close(fd);
    unlink(TEST_FILE);
    return 0;
}

static int test_udp_loopback(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("IO_EVENT_TEST: FAIL udp socket errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(TEST_PORT);

    const char *msg = "io_event_udp";
    if (sendto(fd, msg, strlen(msg), 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("IO_EVENT_TEST: FAIL udp sendto errno=%d\n", errno);
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int test_poll_select_pipe(void)
{
    int p[2];
    if (pipe(p) < 0)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(p[0]);
        usleep(20000);
        if (write(p[1], "p", 1) != 1)
            _exit(1);
        usleep(20000);
        if (write(p[1], "s", 1) != 1)
            _exit(1);
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    struct pollfd pfd = { .fd = p[0], .events = POLLIN };
    char byte;
    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN) ||
        read(p[0], &byte, 1) != 1 || byte != 'p')
        return -1;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    struct timeval timeout = { .tv_sec = 1 };
    if (select(p[0] + 1, &rfds, NULL, NULL, &timeout) != 1 ||
        !FD_ISSET(p[0], &rfds) || read(p[0], &byte, 1) != 1 || byte != 's')
        return -1;

    close(p[0]);
    int status = 0;
    return waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int test_epoll_modes(void)
{
    int efd = eventfd(0, EFD_NONBLOCK);
    int ep = epoll_create1(0);
    if (efd < 0 || ep < 0)
        return -1;
    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = efd,
    };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0)
        return -1;

    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        uint64_t one = 1;
        usleep(20000);
        _exit(write(efd, &one, sizeof(one)) == (ssize_t)sizeof(one) ? 0 : 1);
    }

    struct epoll_event out;
    if (epoll_wait(ep, &out, 1, 1000) != 1 || !(out.events & EPOLLIN) ||
        epoll_wait(ep, &out, 1, 0) != 0)
        return -1;

    uint64_t value;
    if (read(efd, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
        write(efd, &value, sizeof(value)) != (ssize_t)sizeof(value) ||
        epoll_wait(ep, &out, 1, 1000) != 1)
        return -1;

    ev.events = EPOLLIN | EPOLLONESHOT;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) < 0 ||
        epoll_wait(ep, &out, 1, 0) != 1 ||
        epoll_wait(ep, &out, 1, 0) != 0 ||
        epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) < 0 ||
        epoll_wait(ep, &out, 1, 0) != 1)
        return -1;

    int status = 0;
    int ok = waitpid(child, &status, 0) == child &&
             WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok || read(efd, &value, sizeof(value)) != (ssize_t)sizeof(value))
        return -1;

    pid_t ctl_child = fork();
    if (ctl_child < 0)
        return -1;
    if (ctl_child == 0) {
        usleep(20000);
        ev.events = EPOLLOUT;
        _exit(epoll_ctl(ep, EPOLL_CTL_MOD, efd, &ev) == 0 ? 0 : 1);
    }
    if (epoll_wait(ep, &out, 1, 1000) != 1 || !(out.events & EPOLLOUT))
        return -1;
    status = 0;
    ok = waitpid(ctl_child, &status, 0) == ctl_child &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok)
        return -1;

    close(efd);
    int replacement = eventfd(1, EFD_NONBLOCK);
    if (replacement < 0)
        return -1;
    if (replacement != efd) {
        if (dup2(replacement, efd) != efd)
            return -1;
        close(replacement);
    }
    ev.events = EPOLLIN;
    uint64_t replacement_value = 1;
    if (write(efd, &replacement_value, sizeof(replacement_value)) !=
            (ssize_t)sizeof(replacement_value) ||
        epoll_wait(ep, &out, 1, 0) != 0)
        return -1;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0 ||
        epoll_wait(ep, &out, 1, 0) != 1 || !(out.events & EPOLLIN))
        return -1;
    close(ep);
    close(efd);
    return 0;
}

static int test_epoll_watch_lifetime(void)
{
    int p[2];
    int ep = epoll_create1(0);
    if (ep < 0 || pipe(p) < 0)
        return -1;
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 17 };
    struct epoll_event out;
    int duplicate = dup(p[0]);
    if (duplicate < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) < 0)
        return -1;
    close(p[0]);
    if (write(p[1], "d", 1) != 1 || epoll_wait(ep, &out, 1, 500) != 1 ||
        out.data.u32 != 17)
        return -1;
    char byte;
    if (read(duplicate, &byte, 1) != 1 || byte != 'd')
        return -1;
    close(duplicate);
    close(p[1]);
    close(ep);

    /* The read end belongs only to the child after fork. Closing the final
     * writer must still report HUP while the read end is watched; after the
     * last reader closes, the next wait must retire that registration and
     * keep the requested timeout semantics. */
    if (pipe(p) < 0 || (ep = epoll_create1(0)) < 0)
        return -1;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) < 0)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(p[1]);
        usleep(50000);
        close(p[0]);
        _exit(0);
    }
    close(p[0]);
    close(p[1]);
    int ready = epoll_wait(ep, &out, 1, 1000);
    int status = 0;
    int child_ok = waitpid(child, &status, 0) == child &&
                   WIFEXITED(status) && WEXITSTATUS(status) == 0;
    struct timespec start, end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
        return -1;
    int after_close = epoll_wait(ep, &out, 1, 100);
    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0)
        return -1;
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
                      (end.tv_nsec - start.tv_nsec) / 1000000L;
    close(ep);
    if (!child_ok || ready != 1 || !(out.events & EPOLLHUP) ||
        after_close != 0 || elapsed_ms < 75)
        return -1;
    puts("IO_EVENT_TEST: epoll dup, HUP, final-close prune, and fd-reuse PASS");
    return 0;
}

static int test_eventfd_waiters(void)
{
    uint64_t value = UINT64_MAX - 2, increment = 4;
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0 || write(fd, &value, 8) != 8) return -1;
    errno = 0;
    int ok = write(fd, &increment, 8) == -1 && errno == EAGAIN &&
             read(fd, &increment, 8) == 8 && increment == value;
    close(fd);
    if (!ok) return -1;

    /* Completion has a shorter deadline than the children's poll timeout:
     * a lost notification must not pass by rescanning after that timeout. */
    for (int mode = 0; mode < 3; mode++) {
        int ready[2], done[2];
        pid_t children[4] = {0};
        fd = eventfd(0, mode == 1 ? EFD_SEMAPHORE : 0);
        if (fd < 0 || pipe(ready) || pipe(done)) return -1;
        int nchildren = mode == 2 ? 1 : 4;
        if (mode == 2) {
            value = UINT64_MAX - 2;
            if (write(fd, &value, 8) != 8) return -1;
        }
        ok = 1;
        for (int i = 0; i < nchildren; i++) {
            children[i] = fork();
            if (!children[i]) {
                close(ready[0]); close(done[0]);
                alarm(2);
                if (write(ready[1], "r", 1) != 1) _exit(1);
                uint64_t v = 2;
                int result;
                if (mode == 2) result = write(fd, &v, 8) == 8;
                else if (mode == 1) result = read(fd, &v, 8) == 8 && v == 1;
                else {
                    struct pollfd p = {.fd = fd, .events = POLLIN};
                    result = poll(&p, 1, 1000) == 1 && (p.revents & POLLIN);
                }
                if (result) write(done[1], "d", 1);
                _exit(result ? 0 : 1);
            }
            if (children[i] < 0) { ok = 0; break; }
        }
        close(ready[1]); close(done[1]);
        char c;
        for (int i = 0; ok && i < nchildren; i++)
            ok = read(ready[0], &c, 1) == 1;
        usleep(50000);
        value = mode == 1 ? 4 : 1;
        if (ok) ok = mode == 2 ? read(fd, &value, 8) == 8
                               : write(fd, &value, 8) == 8;
        for (int i = 0; ok && i < nchildren; i++) {
            struct pollfd p = {.fd = done[0], .events = POLLIN};
            ok = poll(&p, 1, 300) == 1 && read(done[0], &c, 1) == 1;
        }
        for (int i = 0; i < nchildren; i++) if (children[i] > 0) {
            if (!ok) kill(children[i], SIGKILL);
            int status;
            if (waitpid(children[i], &status, 0) != children[i] ||
                !WIFEXITED(status) || WEXITSTATUS(status)) ok = 0;
        }
        close(fd); close(ready[0]); close(done[0]);
        if (!ok) return -1;
    }
    puts("IO_EVENT_TEST: eventfd overflow+observers+semaphore+writer PASS");
    return 0;
}

static int test_timerfd_poll(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0)
        return -1;
    struct itimerspec timer = { .it_value = { .tv_nsec = 20000000 } };
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t expirations;
    int ok = timerfd_settime(fd, 0, &timer, NULL) == 0 &&
             poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN) &&
             read(fd, &expirations, sizeof(expirations)) ==
                 (ssize_t)sizeof(expirations) && expirations >= 1;
    if (!ok) {
        close(fd);
        return -1;
    }

    struct itimerspec disarm = {0};
    if (timerfd_settime(fd, 0, &disarm, NULL) < 0) {
        close(fd);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(fd);
        return -1;
    }
    if (child == 0) {
        usleep(20000);
        _exit(timerfd_settime(fd, 0, &timer, NULL) == 0 ? 0 : 1);
    }
    pfd.revents = 0;
    ok = poll(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN) &&
         read(fd, &expirations, sizeof(expirations)) ==
             (ssize_t)sizeof(expirations) && expirations >= 1;
    int status = 0;
    ok = ok && waitpid(child, &status, 0) == child &&
         WIFEXITED(status) && WEXITSTATUS(status) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int main(void)
{
    if (test_block_io() < 0)
        return 1;
    if (test_udp_loopback() < 0)
        return 1;
    if (test_poll_select_pipe() < 0) {
        printf("IO_EVENT_TEST: FAIL poll/select errno=%d\n", errno);
        return 1;
    }
    if (test_epoll_modes() < 0) {
        printf("IO_EVENT_TEST: FAIL epoll modes errno=%d\n", errno);
        return 1;
    }
    if (test_epoll_watch_lifetime() < 0) {
        printf("IO_EVENT_TEST: FAIL epoll watch lifetime errno=%d\n", errno);
        return 1;
    }
    if (test_eventfd_waiters() < 0) {
        printf("IO_EVENT_TEST: FAIL eventfd waiters errno=%d\n", errno);
        return 1;
    }
    if (test_timerfd_poll() < 0) {
        printf("IO_EVENT_TEST: FAIL timerfd errno=%d\n", errno);
        return 1;
    }

    printf("IO_EVENT_TEST: PASS\n");
    return 0;
}
