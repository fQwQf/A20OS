#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
    if (epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0 ||
        epoll_wait(ep, &out, 1, 0) != 1 || !(out.events & EPOLLIN))
        return -1;
    close(ep);
    close(efd);
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
    if (test_timerfd_poll() < 0) {
        printf("IO_EVENT_TEST: FAIL timerfd errno=%d\n", errno);
        return 1;
    }

    printf("IO_EVENT_TEST: PASS\n");
    return 0;
}
