#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t handler_fired = 0;

static void usr1_handler(int sig)
{
    (void)sig;
    handler_fired = 1;
}

static int fail(const char *what)
{
    printf("SIGNALFD_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int set_mask(sigset_t *mask, int sig)
{
    sigemptyset(mask);
    sigaddset(mask, sig);
    return sigprocmask(SIG_BLOCK, mask, NULL);
}

int main(void)
{
    sigset_t mask;
    struct signalfd_siginfo fdsi;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
        return fail("sigaction");

    if (set_mask(&mask, SIGUSR1) < 0)
        return fail("sigprocmask");

    int fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (fd < 0)
        return fail("signalfd create");

    if (read(fd, &fdsi, sizeof(fdsi)) >= 0 || errno != EAGAIN)
        return fail("empty read must EAGAIN");

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) != 0)
        return fail("empty poll must be 0");

    if (kill(getpid(), SIGUSR1) < 0)
        return fail("kill self");

    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN))
        return fail("poll after signal");

    ssize_t n = read(fd, &fdsi, sizeof(fdsi));
    if (n != (ssize_t)sizeof(fdsi))
        return fail("read size");
    if (fdsi.ssi_signo != SIGUSR1)
        return fail("ssi_signo");
    if (fdsi.ssi_pid != (uint32_t)getpid())
        return fail("ssi_pid");
    if (handler_fired)
        return fail("handler must not run for signalfd-consumed signal");

    sigset_t pending;
    sigemptyset(&pending);
    if (sigpending(&pending) < 0 || sigismember(&pending, SIGUSR1))
        return fail("signal must be dequeued");

    /* Mask update on an existing fd. */
    sigset_t mask2;
    if (set_mask(&mask2, SIGUSR2) < 0)
        return fail("sigprocmask2");
    if (signalfd(fd, &mask2, 0) != fd)
        return fail("signalfd update");
    if (kill(getpid(), SIGUSR1) < 0)
        return fail("kill usr1");
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0)
        return fail("old mask must not match");
    if (kill(getpid(), SIGUSR2) < 0)
        return fail("kill usr2");
    if (read(fd, &fdsi, sizeof(fdsi)) != (ssize_t)sizeof(fdsi) ||
        fdsi.ssi_signo != SIGUSR2)
        return fail("updated mask read");

    /* Blocking read wakes when a signal arrives. */
    int bfd = signalfd(-1, &mask2, 0);
    if (bfd < 0)
        return fail("signalfd blocking create");
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    if (read(bfd, &fdsi, sizeof(fdsi)) != (ssize_t)sizeof(fdsi) ||
        fdsi.ssi_signo != SIGUSR2)
        return fail("blocking read");
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status))
        return fail("waitpid");
    close(bfd);

    /* epoll integration. */
    int ep = epoll_create1(0);
    if (ep < 0)
        return fail("epoll_create1");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0)
        return fail("epoll_ctl");
    if (kill(getpid(), SIGUSR2) < 0)
        return fail("kill usr2 again");
    struct epoll_event out;
    if (epoll_wait(ep, &out, 1, 1000) != 1 || !(out.events & EPOLLIN))
        return fail("epoll_wait");
    if (read(fd, &fdsi, sizeof(fdsi)) != (ssize_t)sizeof(fdsi))
        return fail("drain after epoll");
    close(ep);
    close(fd);

    /* The stray SIGUSR1 sent earlier is still pending and blocked. */
    if (sigpending(&pending) < 0 || !sigismember(&pending, SIGUSR1))
        return fail("non-masked signal must stay pending");

    printf("SIGNALFD_STRESS: PASS\n");
    return 0;
}
