#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t hung, continued;
static void on_signal(int sig) { if (sig == SIGHUP) hung = 1; else continued = 1; }
static void fail(const char *what) {
    dprintf(2, "TTY_LIFECYCLE: FAIL %s errno=%d\n", what, errno);
    _exit(1);
}
#define CHECK(c, w) do { if (!(c)) fail(w); } while (0)
static void receive(int fd, char expected) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int r;
    do { r = poll(&p, 1, 5000); } while (r < 0 && errno == EINTR);
    char c;
    CHECK(r == 1 && read(fd, &c, 1) == 1 && c == expected, "bounded handshake");
}
static int pair(int *master, char *path) {
    *master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(*master >= 0 && grantpt(*master) == 0 && unlockpt(*master) == 0, "master");
    CHECK(ptsname_r(*master, path, 128) == 0, "slave name");
    int fd = open(path, O_RDWR | O_NOCTTY);
    CHECK(fd >= 0, "slave");
    return fd;
}
static void exited(pid_t pid) {
    int status;
    pid_t r;
    do { r = waitpid(pid, &status, 0); } while (r < 0 && errno == EINTR);
    CHECK(r == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit");
}
enum { DETACH, LEADER_EXIT, MASTER_CLOSE, VHANGUP, BLOCKED_READER };
static void lifecycle(int action) {
    int report[2];
    CHECK(pipe(report) == 0, "report pipe");
    pid_t leader = fork();
    CHECK(leader >= 0, "fork leader");
    if (!leader) {
        close(report[0]);
        CHECK(setsid() == getpid(), "new session");
        int master; char path[128];
        int slave = pair(&master, path);
        CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "claim tty");
        int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
        CHECK(tty >= 0 && tcgetpgrp(tty) == getpgrp(), "dev tty follows session");
        close(tty);
        signal(SIGHUP, on_signal);
        signal(SIGCONT, on_signal);
        int ready[2], go[2];
        CHECK(pipe(ready) == 0 && pipe(go) == 0, "ready pipe");
        pid_t worker = fork();
        CHECK(worker >= 0, "fork worker");
        if (!worker) {
            close(ready[0]); close(go[1]);
            if (action != LEADER_EXIT) close(master);
            CHECK(setpgid(0, 0) == 0, "worker group");
            struct sigaction sa = {0};
            sa.sa_handler = on_signal;
            sigemptyset(&sa.sa_mask);
            CHECK(sigaction(SIGHUP, &sa, NULL) == 0 && sigaction(SIGCONT, &sa, NULL) == 0,
                  "hangup handlers");
            CHECK(write(ready[1], "R", 1) == 1, "worker ready");
            if (action == BLOCKED_READER) {
                /* Parent makes us foreground before releasing this stop. */
                raise(SIGSTOP);
                CHECK(write(ready[1], "B", 1) == 1, "reader about to block");
                char c; ssize_t n;
                do { n = read(slave, &c, 1); } while (n < 0 && errno == EINTR);
                CHECK(n == 0, "revoked blocked reader gets EOF");
            } else if (action == DETACH || action == LEADER_EXIT) {
                raise(SIGSTOP);
                CHECK(hung && continued, "foreground receives HUP and CONT");
            } else receive(go[0], 'G');
            errno = 0;
            CHECK(tcgetpgrp(slave) == -1 && errno == (action == DETACH || action == LEADER_EXIT ? ENOTTY : EIO), "session loses controlling tty");
            errno = 0;
            CHECK(open("/dev/tty", O_RDWR | O_NOCTTY) == -1 && errno == ENXIO,
                  "dev tty absent after hangup");
            CHECK(write(report[1], "P", 1) == 1, "worker result");
            _exit(0);
        }
        close(ready[1]); close(go[0]);
        receive(ready[0], 'R');
        int status;
        if (action == DETACH || action == LEADER_EXIT || action == BLOCKED_READER)
            CHECK(waitpid(worker, &status, WUNTRACED) == worker && WIFSTOPPED(status), "worker stopped");
        CHECK(tcsetpgrp(slave, worker) == 0, "worker foreground");
        sigset_t blocked;
        sigemptyset(&blocked); sigaddset(&blocked, SIGTTOU);
        CHECK(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0, "block background ioctl signal");
        if (action == LEADER_EXIT) _exit(0);
        if (action == DETACH) CHECK(ioctl(slave, TIOCNOTTY) == 0, "detach leader");
        if (action == MASTER_CLOSE) { close(master); master = -1; }
        if (action == BLOCKED_READER) {
            CHECK(kill(worker, SIGCONT) == 0, "continue reader");
            receive(ready[0], 'B');
            usleep(20000);
        }
        if (action == VHANGUP || action == BLOCKED_READER) {
            CHECK(vhangup() == 0, "privileged vhangup");
            char c;
            CHECK(read(slave, &c, 1) == 0, "old fd EOF");
            errno = 0;
            CHECK(write(slave, "X", 1) == -1 && errno == EIO, "old fd write EIO");
            struct pollfd revoked = {.fd = slave, .events = POLLIN | POLLOUT};
            CHECK(poll(&revoked, 1, 0) == 1 &&
                  (revoked.revents & (POLLHUP | POLLERR)) == (POLLHUP | POLLERR),
                  "old fd poll hangup");
            struct termios modes;
            errno = 0;
            CHECK(tcgetattr(slave, &modes) == -1 && errno == EIO, "old fd ioctl EIO");
            int fresh = open(path, O_RDWR | O_NOCTTY);
            CHECK(fresh >= 0 && ioctl(fresh, TIOCSCTTY, 0) == 0, "fresh open reacquires tty");
            errno = 0;
            CHECK(write(slave, "X", 1) == -1 && errno == EIO,
                  "fresh open does not restore old fd authority");
            signal(SIGHUP, SIG_IGN);
            CHECK(ioctl(fresh, TIOCNOTTY) == 0, "detach fresh tty");
            close(fresh);
        }
        if (action == MASTER_CLOSE || action == VHANGUP) {
            CHECK(hung && continued, "hangup signals session leader");
            CHECK(write(go[1], "G", 1) == 1, "release observer");
        }
        exited(worker);
        close(slave); if (master >= 0) close(master);
        _exit(0);
    }
    close(report[1]);
    receive(report[0], 'P'); close(report[0]);
    exited(leader);
}
static void steal(void) {
    CHECK(setsid() == getpid(), "steal owner session");
    int master; char path[128];
    int slave = pair(&master, path);
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "steal initial owner");
    pid_t child = fork();
    CHECK(child >= 0, "fork unprivileged thief");
    if (!child) {
        CHECK(setsid() == getpid() && setuid(65534) == 0, "drop thief privilege");
        errno = 0;
        CHECK(ioctl(slave, TIOCSCTTY, 1) == -1 && errno == EPERM, "unprivileged steal refused");
        errno = 0;
        CHECK(vhangup() == -1 && errno == EPERM, "unprivileged vhangup refused");
        _exit(0);
    }
    exited(child);
    CHECK(tcgetpgrp(slave) == getpgrp(), "failed thief leaves owner intact");
    child = fork();
    CHECK(child >= 0, "fork privileged thief");
    if (!child) {
        CHECK(setsid() == getpid(), "thief session");
        errno = 0;
        CHECK(ioctl(slave, TIOCSCTTY, 0) == -1 && errno == EPERM, "steal flag required");
        CHECK(ioctl(slave, TIOCSCTTY, 1) == 0, "privileged steal succeeds");
        int sid;
        CHECK(ioctl(slave, TIOCGSID, &sid) == 0 && sid == getpid(), "thief owns tty");
        signal(SIGHUP, SIG_IGN);
        _exit(0);
    }
    exited(child);
    errno = 0;
    CHECK(tcgetpgrp(slave) == -1 && errno == ENOTTY, "old session ownership revoked");
    char bytes[2];
    CHECK(write(master, "S\n", 2) == 2 && read(slave, bytes, 2) == 2 &&
          !memcmp(bytes, "S\n", 2), "steal preserves ordinary fd I/O");
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "old session can acquire released tty");
    signal(SIGHUP, SIG_IGN);
    CHECK(ioctl(slave, TIOCNOTTY) == 0, "steal cleanup");
    close(slave); close(master);
}
static void *thread_detach(void *arg) {
    int slave = *(int *)arg;
    CHECK(tcgetpgrp(slave) == getpgrp(), "thread shares controlling tty");
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "leader thread group repeats claim");
    CHECK(ioctl(slave, TIOCNOTTY) == 0, "thread detaches process");
    return NULL;
}
static void threaded(void) {
    CHECK(setsid() == getpid(), "thread session");
    int master; char path[128];
    int slave = pair(&master, path);
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "thread initial owner");
    signal(SIGHUP, SIG_IGN);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, thread_detach, &slave) == 0, "create thread");
    CHECK(pthread_join(worker, NULL) == 0, "join thread");
    errno = 0;
    CHECK(tcgetpgrp(slave) == -1 && errno == ENOTTY, "thread detach visible to whole process");
    close(slave); close(master);
}
static void orphan_access(void) {
    CHECK(setsid() == getpid(), "orphan test session");
    int master; char path[128];
    int slave = pair(&master, path);
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "orphan test owner");
    struct termios modes;
    CHECK(tcgetattr(slave, &modes) == 0, "orphan modes");
    modes.c_lflag |= TOSTOP;
    CHECK(tcsetattr(slave, TCSANOW, &modes) == 0, "orphan TOSTOP");
    int go[2], report[2];
    CHECK(pipe(go) == 0 && pipe(report) == 0, "orphan pipes");
    pid_t middle = fork();
    CHECK(middle >= 0, "fork orphan parent");
    if (!middle) {
        pid_t child = fork();
        CHECK(child >= 0, "fork orphan worker");
        if (child) _exit(0);
        close(go[1]); close(report[0]); close(master);
        CHECK(setpgid(0, 0) == 0, "orphan worker group");
        receive(go[0], 'G');
        char c;
        errno = 0;
        CHECK(read(slave, &c, 1) == -1 && errno == EIO, "orphan background read EIO");
        errno = 0;
        CHECK(write(slave, "X", 1) == -1 && errno == EIO, "orphan background write EIO");
        errno = 0;
        CHECK(tcsetattr(slave, TCSANOW, &modes) == -1 && errno == EIO, "orphan background ioctl EIO");
        CHECK(write(report[1], "P", 1) == 1, "orphan result");
        _exit(0);
    }
    close(go[0]); close(report[1]);
    exited(middle);
    CHECK(write(go[1], "G", 1) == 1, "orphan parent has exited");
    receive(report[0], 'P');
    close(go[1]); close(report[0]);
    signal(SIGHUP, SIG_IGN);
    CHECK(ioctl(slave, TIOCNOTTY) == 0, "orphan test detach");
    close(slave); close(master);
}
static void automatic_acquire(void) {
    CHECK(setsid() == getpid(), "automatic session");
    int master; char path[128];
    int slave = pair(&master, path);
    errno = 0;
    CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO, "O_NOCTTY prevents acquisition");
    int writer = open(path, O_WRONLY);
    CHECK(writer >= 0, "write-only slave open");
    errno = 0;
    CHECK(open("/dev/tty", O_RDWR) == -1 && errno == ENXIO, "write-only open does not acquire");
    close(writer);
    int reader = open(path, O_RDWR);
    CHECK(reader >= 0 && tcgetpgrp(reader) == getpgrp(), "readable open automatically acquires");
    int current = open("/dev/tty", O_RDWR);
    CHECK(current >= 0 && tcgetpgrp(current) == getpgrp(), "dev tty resolves acquired slave");
    close(current);
    signal(SIGHUP, SIG_IGN);
    CHECK(ioctl(reader, TIOCNOTTY) == 0, "automatic detach");
    close(reader); close(slave); close(master);
}
static void private_session(void (*fn)(void)) {
    pid_t child = fork();
    CHECK(child >= 0, "fork private session");
    if (!child) { fn(); _exit(0); }
    exited(child);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--revocation")) {
        lifecycle(VHANGUP); puts("TTY_LIFECYCLE: vhangup PASS");
        lifecycle(BLOCKED_READER); puts("TTY_LIFECYCLE: blocked-reader PASS");
        private_session(steal); puts("TTY_LIFECYCLE: privileged-steal PASS");
    } else {
        lifecycle(DETACH); puts("TTY_LIFECYCLE: detach-signals PASS");
        lifecycle(LEADER_EXIT); puts("TTY_LIFECYCLE: leader-exit PASS");
        lifecycle(MASTER_CLOSE); puts("TTY_LIFECYCLE: master-close PASS");
        private_session(threaded); puts("TTY_LIFECYCLE: thread-sharing PASS");
        private_session(orphan_access); puts("TTY_LIFECYCLE: orphan-access PASS");
        private_session(automatic_acquire); puts("TTY_LIFECYCLE: automatic-acquire PASS");
    }
    puts("TTY_LIFECYCLE: PASS");
    return 0;
}
