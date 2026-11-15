#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static pid_t foreign_group;

static void fail(const char *what)
{
    dprintf(2, "TTY_JOBCTRL: FAIL %s errno=%d\n", what, errno);
    _exit(1);
}
#define CHECK(condition, what) do { if (!(condition)) fail(what); } while (0)

static unsigned long long milliseconds(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0, "clock");
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_event(pid_t child, int stopped)
{
    unsigned long long until = milliseconds() + 5000;
    while (milliseconds() < until) {
        int status = 0;
        pid_t got = waitpid(child, &status, WNOHANG | WUNTRACED);
        if (got < 0 && errno == EINTR) continue;
        CHECK(got >= 0, "waitpid");
        if (got == child) {
            if (stopped) {
                CHECK(WIFSTOPPED(status), "expected job-control stop");
                return WSTOPSIG(status);
            }
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child result");
            return 0;
        }
        usleep(1000);
    }
    kill(child, SIGKILL);
    fail("child deadline");
    return -1;
}

static int pair(int *master)
{
    *master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(*master >= 0, "open ptmx");
    CHECK(grantpt(*master) == 0 && unlockpt(*master) == 0, "unlock ptmx");
    char path[128];
    CHECK(ptsname_r(*master, path, sizeof(path)) == 0, "ptsname");
    int slave = open(path, O_RDWR | O_NOCTTY);
    CHECK(slave >= 0, "open slave");
    return slave;
}

static void block_signal(int sig, int block)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, sig);
    CHECK(sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &mask, NULL) == 0, "signal mask");
}

static void run_session(int identity_only)
{
    CHECK(setsid() == getpid(), "setsid");
    int master, second_master;
    int slave = pair(&master), second = pair(&second_master);
    int first_number = -1, second_number = -1;
    CHECK(ioctl(master, TIOCGPTN, &first_number) == 0 &&
          ioctl(second_master, TIOCGPTN, &second_number) == 0 &&
          first_number != second_number, "distinct pty identities");
    const char *messages[2] = {"Aa\n", "Bb\n"};
    int masters[2] = {master, second_master};
    int slaves[2] = {slave, second};
    for (int i = 0; i < 2; i++) {
        char bytes[3];
        CHECK(write(masters[i], messages[i], 3) == 3 &&
              read(slaves[i], bytes, sizeof(bytes)) == 3 &&
              memcmp(bytes, messages[i], 3) == 0, "independent pty streams");
    }
    int null = open("/dev/null", O_RDWR);
    CHECK(null >= 0, "open non-tty");
    errno = 0;
    CHECK(ioctl(null, TIOCGPTN, &first_number) == -1 && errno == ENOTTY,
          "non-pty number query rejected");
    unsigned long controls[] = {TIOCSCTTY, TIOCNOTTY, TIOCGPGRP, TIOCSPGRP, TIOCGSID};
    for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        int group = getpgrp();
        errno = 0;
        CHECK(ioctl(null, controls[i], &group) == -1 && errno == ENOTTY,
              "non-terminal control rejected");
    }
    int locked = 1;
    CHECK(ioctl(master, TIOCSPTLCK, &locked) == 0, "lock pty slave");
    char locked_path[128];
    CHECK(ptsname_r(master, locked_path, sizeof(locked_path)) == 0, "locked pty name");
    errno = 0;
    int denied = open(locked_path, O_RDWR | O_NOCTTY);
    CHECK(denied == -1 && errno == EIO, "locked slave rejects another open");
    locked = 0;
    CHECK(ioctl(master, TIOCSPTLCK, &locked) == 0, "unlock pty slave again");
    denied = open(locked_path, O_RDWR | O_NOCTTY);
    CHECK(denied >= 0, "unlocked slave reopens");
    close(denied); close(null);
    if (identity_only) {
        close(second); close(second_master); close(slave); close(master);
        puts("TTY_JOBCTRL: pty-identity PASS");
        return;
    }
    errno = 0;
    CHECK(tcgetpgrp(slave) == -1 && errno == ENOTTY, "unowned slave has no controlling group");

    pid_t child = fork();
    CHECK(child >= 0, "fork nonleader");
    if (!child) {
        errno = 0;
        CHECK(ioctl(slave, TIOCSCTTY, 0) == -1 && errno == EPERM,
              "non-session-leader claim rejected");
        _exit(0);
    }
    wait_event(child, 0);
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "claim controlling terminal");
    CHECK(ioctl(slave, TIOCSCTTY, 0) == 0, "same-terminal claim is idempotent");
    errno = 0;
    CHECK(ioctl(second, TIOCSCTTY, 0) == -1 && errno == EPERM, "second terminal rejected");
    int sid = -1;
    CHECK(ioctl(slave, TIOCGSID, &sid) == 0 && sid == getsid(0), "terminal session identity");
    CHECK(tcgetpgrp(slave) == getpgrp(), "initial foreground group");
    errno = 0;
    CHECK(tcsetpgrp(slave, 1000000000) == -1 && errno == ESRCH, "nonexistent foreground rejected");
    errno = 0;
    CHECK(tcsetpgrp(slave, foreign_group) == -1 && errno == EPERM, "foreign-session group rejected");
    puts("TTY_JOBCTRL: ownership PASS");

    /* A blocked parent SIGTTOU permits giving the terminal back to itself
     * after a child temporarily owns the foreground. Children explicitly
     * unblock it when exercising the stopping behavior. */
    block_signal(SIGTTOU, 1);
    child = fork();
    CHECK(child >= 0, "fork reader");
    if (!child) {
        CHECK(setpgid(0, 0) == 0, "reader process group");
        char bytes[2];
        ssize_t got;
        do { got = read(slave, bytes, sizeof(bytes)); } while (got < 0 && errno == EINTR);
        CHECK(got == 2 && bytes[0] == 'R' && bytes[1] == '\n', "foreground reader resumed");
        _exit(0);
    }
    CHECK(wait_event(child, 1) == SIGTTIN, "background read SIGTTIN");
    CHECK(tcsetpgrp(slave, child) == 0, "reader becomes foreground");
    CHECK(kill(child, SIGCONT) == 0 && write(master, "R\n", 2) == 2, "reader continue and input");
    wait_event(child, 0);
    CHECK(tcsetpgrp(slave, getpgrp()) == 0, "restore foreground");
    puts("TTY_JOBCTRL: background-read PASS");

    for (int ignore = 0; ignore < 2; ignore++) {
        child = fork();
        CHECK(child >= 0, "fork blocked reader");
        if (!child) {
            CHECK(setpgid(0, 0) == 0, "blocked reader process group");
            if (ignore) signal(SIGTTIN, SIG_IGN);
            else block_signal(SIGTTIN, 1);
            char byte;
            errno = 0;
            CHECK(read(slave, &byte, 1) == -1 && errno == EIO, "blocked/ignored TTIN gives EIO");
            _exit(0);
        }
        wait_event(child, 0);
    }

    struct termios modes;
    CHECK(tcgetattr(slave, &modes) == 0, "get modes");
    child = fork();
    CHECK(child >= 0, "fork ioctl writer");
    if (!child) {
        CHECK(setpgid(0, 0) == 0, "ioctl writer group");
        block_signal(SIGTTOU, 0);
        int result;
        do { result = tcsetattr(slave, TCSANOW, &modes); } while (result < 0 && errno == EINTR);
        CHECK(result == 0, "foreground ioctl resumed");
        _exit(0);
    }
    CHECK(wait_event(child, 1) == SIGTTOU, "background ioctl signals without TOSTOP");
    CHECK(tcsetpgrp(slave, child) == 0 && kill(child, SIGCONT) == 0, "ioctl writer foreground");
    wait_event(child, 0);
    CHECK(tcsetpgrp(slave, getpgrp()) == 0, "restore ioctl foreground");
    puts("TTY_JOBCTRL: background-ioctl PASS");
    modes.c_lflag |= TOSTOP;
    CHECK(tcsetattr(slave, TCSANOW, &modes) == 0, "set TOSTOP");
    child = fork();
    CHECK(child >= 0, "fork writer");
    if (!child) {
        CHECK(setpgid(0, 0) == 0, "writer process group");
        block_signal(SIGTTOU, 0);
        ssize_t got;
        do { got = write(slave, "W", 1); } while (got < 0 && errno == EINTR);
        CHECK(got == 1, "foreground writer resumed");
        _exit(0);
    }
    CHECK(wait_event(child, 1) == SIGTTOU, "TOSTOP sends SIGTTOU");
    CHECK(tcsetpgrp(slave, child) == 0 && kill(child, SIGCONT) == 0, "foreground writer continue");
    wait_event(child, 0);
    CHECK(tcsetpgrp(slave, getpgrp()) == 0, "restore writer foreground");
    for (int ignore = 0; ignore < 2; ignore++) {
        child = fork();
        CHECK(child >= 0, "fork permitted writer");
        if (!child) {
            CHECK(setpgid(0, 0) == 0, "permitted writer group");
            if (ignore) { block_signal(SIGTTOU, 0); signal(SIGTTOU, SIG_IGN); }
            CHECK(write(slave, "P", 1) == 1, "blocked/ignored TTOU allows write");
            _exit(0);
        }
        wait_event(child, 0);
    }
    modes.c_lflag &= ~TOSTOP;
    CHECK(tcsetattr(slave, TCSANOW, &modes) == 0, "clear TOSTOP");
    puts("TTY_JOBCTRL: background-write PASS");

    child = fork();
    CHECK(child >= 0, "fork detach child");
    if (!child) {
        CHECK(ioctl(slave, TIOCNOTTY) == 0, "nonleader detach");
        errno = 0;
        CHECK(tcgetpgrp(slave) == -1 && errno == ENOTTY, "detached child has no tty");
        _exit(0);
    }
    wait_event(child, 0);
    CHECK(tcgetpgrp(slave) == getpgrp(), "child detach preserves parent tty");
    signal(SIGHUP, SIG_IGN);
    CHECK(ioctl(slave, TIOCNOTTY) == 0, "leader detach");
    errno = 0;
    CHECK(tcgetpgrp(slave) == -1 && errno == ENOTTY, "leader lost tty");
    close(second); close(second_master); close(slave); close(master);
    puts("TTY_JOBCTRL: detach PASS");
}

int main(int argc, char **argv)
{
    foreign_group = getpgrp();
    setvbuf(stdout, NULL, _IONBF, 0);
    pid_t child = fork();
    CHECK(child >= 0, "fork private session");
    if (!child) {
        run_session(argc == 2 && !strcmp(argv[1], "--identity"));
        _exit(0);
    }
    wait_event(child, 0);
    puts(argc == 2 && !strcmp(argv[1], "--identity") ? "TTY_JOBCTRL: identity PASS" : "TTY_JOBCTRL: PASS");
    return 0;
}
