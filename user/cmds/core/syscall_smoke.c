#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <pthread.h>
#include <poll.h>

static int fail(const char *what)
{
    printf("SYSCALL_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static int thread_wait_pipe[2];
static int thread_wait_pid;
static int thread_wait_pidfd;
static int thread_wait_error;
static int zombie_group_release_fd;

static void *zombie_group_member(void *unused)
{
    (void)unused;
    char byte;
    while (read(zombie_group_release_fd, &byte, 1) < 0 && errno == EINTR)
        ;
    syscall(SYS_exit, 0);
    return NULL;
}

static void zombie_group_child(int ready_fd, int release_fd, int exit_fd)
{
    zombie_group_release_fd = release_fd;
    pthread_t member;
    if (pthread_create(&member, NULL, zombie_group_member, NULL) != 0)
        _exit(70);
    if (write(ready_fd, "r", 1) != 1)
        _exit(71);
    char exit_byte;
    if (read(exit_fd, &exit_byte, 1) != 1)
        _exit(72);
    /* Exit just this thread, leaving a live member behind.  libc _exit uses
     * exit_group and would terminate the scenario instead. */
    syscall(SYS_exit, 23);
    _exit(73);
}

static int proc_pid_state(pid_t pid)
{
    char path[64];
    char line[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    char *comm_end = strrchr(line, ')');
    return comm_end && comm_end[1] == ' ' ? comm_end[2] : -1;
}

static int test_wait_zombie_leader_with_live_member(void)
{
    int ready_pipe[2], release_pipe[2], exit_pipe[2];
    if (pipe(ready_pipe) < 0 || pipe(release_pipe) < 0 || pipe(exit_pipe) < 0)
        return fail("wait-live-group-pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("wait-live-group-fork");
    if (pid == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        close(exit_pipe[1]);
        zombie_group_child(ready_pipe[1], release_pipe[0], exit_pipe[0]);
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    close(exit_pipe[0]);
    char byte;
    if (read(ready_pipe[0], &byte, 1) != 1) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        close(exit_pipe[1]);
        return fail("wait-live-group-ready");
    }
    close(ready_pipe[0]);
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        close(exit_pipe[1]);
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-pidfd");
    }

    if (write(exit_pipe[1], "x", 1) != 1) {
        close(exit_pipe[1]);
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-exit-leader");
    }
    close(exit_pipe[1]);
    sched_yield();

    /* Wait until procfs confirms the leader is zombie.  The group member
     * remains blocked on release_pipe, so pidfd must still be unreadable. */
    int leader_zombie = 0;
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (proc_pid_state(pid) == 'Z') {
            leader_zombie = 1;
            break;
        }
        sched_yield();
    }
    if (!leader_zombie) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-leader-zombie-timeout");
    }

    int zombie_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (zombie_pidfd < 0) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-open-zombie-pidfd");
    }
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN, .revents = 0};
    int polled = poll(&pfd, 1, 0);
    close(zombie_pidfd);
    if (polled != 0 || pfd.revents != 0) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-pidfd-early-readable");
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED | WNOHANG) < 0 ||
        info.si_pid != 0) {
        int saved_errno = errno;
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return fail("wait-live-group-nohang");
    }

    if (write(release_pipe[1], "x", 1) != 1) {
        close(release_pipe[1]);
        close(pidfd);
        return fail("wait-live-group-release");
    }
    close(release_pipe[1]);
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED | WNOWAIT) < 0) {
        close(pidfd);
        return fail("wait-live-group-wnowait");
    }
    if (info.si_pid != pid || info.si_status != 23) {
        close(pidfd);
        return fail("wait-live-group-final-info");
    }
    pfd.revents = 0;
    polled = poll(&pfd, 1, 0);
    if (polled != 1 || !(pfd.revents & POLLIN)) {
        close(pidfd);
        return fail("wait-live-group-pidfd-readable");
    }
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED) < 0) {
        close(pidfd);
        return fail("wait-live-group-reap");
    }
    close(pidfd);
    return 0;
}

/* Create a process from a non-leader thread, then let that thread exit.  The
 * main thread must still be able to wait for its thread-group child's pidfd. */
static void *thread_spawn_child(void *unused)
{
    (void)unused;
    pid_t pid = fork();
    if (pid < 0) {
        thread_wait_error = errno ? errno : ECHILD;
        return NULL;
    }
    if (pid == 0) {
        close(thread_wait_pipe[1]);
        char byte;
        if (read(thread_wait_pipe[0], &byte, 1) != 1)
            _exit(62);
        _exit(61);
    }

    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        thread_wait_error = errno ? errno : ECHILD;
        return NULL;
    }
    thread_wait_pid = (int)pid;
    thread_wait_pidfd = pidfd;
    return NULL;
}

static int test_thread_group_pidfd_wait(void)
{
    if (pipe(thread_wait_pipe) < 0)
        return fail("thread-wait-pipe");
    thread_wait_pid = -1;
    thread_wait_pidfd = -1;
    thread_wait_error = 0;

    pthread_t helper;
    int err = pthread_create(&helper, NULL, thread_spawn_child, NULL);
    if (err != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        errno = err;
        return fail("thread-wait-create");
    }
    err = pthread_join(helper, NULL);
    if (err != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        errno = err;
        return fail("thread-wait-join");
    }
    if (thread_wait_error || thread_wait_pid <= 0 || thread_wait_pidfd < 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        errno = thread_wait_error ? thread_wait_error : ECHILD;
        return fail("thread-wait-spawn");
    }

    siginfo_t info;
    memset(&info, 0xa5, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info,
               WEXITED | WNOHANG) < 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-live-child");
    }
    if (info.si_pid != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-nohang-result");
    }

    if (write(thread_wait_pipe[1], "x", 1) != 1) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-release-child");
    }
    close(thread_wait_pipe[0]);
    close(thread_wait_pipe[1]);

    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info,
               WEXITED | WNOWAIT) < 0)
        return fail("thread-wait-wnowait");
    if (info.si_signo != SIGCHLD || info.si_code != CLD_EXITED ||
        info.si_pid != thread_wait_pid || info.si_status != 61)
        return fail("thread-wait-wnowait-info");

    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info, WEXITED) < 0)
        return fail("thread-wait-reap");
    if (info.si_signo != SIGCHLD || info.si_pid != thread_wait_pid ||
        info.si_status != 61)
        return fail("thread-wait-reap-info");
    close(thread_wait_pidfd);

    errno = 0;
    if (waitpid(thread_wait_pid, NULL, WNOHANG) != -1 || errno != ECHILD)
        return fail("thread-wait-reaped-echild");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc != 3 || !argv[1] || !argv[2] ||
            strcmp(argv[1], "exec-argv") != 0 ||
            strcmp(argv[2], "sentinel") != 0)
            return fail("exec-argv");
        return 0;
    }

    printf("SYSCALL_SMOKE: start\n");

    if (getpid() <= 0)
        return fail("getpid");

    int pfd[2];
    if (pipe(pfd) < 0)
        return fail("pipe");
    const char *msg = "a20-linux-abi";
    char buf[64];
    if (write(pfd[1], msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("pipe-write");
    int n = read(pfd[0], buf, sizeof(buf) - 1);
    if (n < 0)
        return fail("pipe-read");
    buf[n] = '\0';
    close(pfd[0]);
    close(pfd[1]);
    if (strcmp(buf, msg) != 0)
        return fail("pipe-compare");

    const char *path = "/tmp/syscall_smoke.txt";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("open");
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("write");
    if (lseek(fd, 0, SEEK_SET) < 0)
        return fail("lseek");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0)
        return fail("read");
    close(fd);
    if (strcmp(buf, msg) != 0)
        return fail("file-compare");

    struct stat st;
    if (stat(path, &st) < 0 || st.st_size != (off_t)strlen(msg))
        return fail("stat");

    /* renameat(2): glibc calls it directly; verify the rename + new path. */
#ifndef SYS_renameat
#define SYS_renameat 38
#endif
    char oldpath[64], newpath[64];
    snprintf(oldpath, sizeof(oldpath), "%s.old", path);
    snprintf(newpath, sizeof(newpath), "%s.new", path);
    int ofd = open(oldpath, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (ofd < 0)
        return fail("renameat-create");
    if (write(ofd, msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("renameat-write");
    close(ofd);
    if (syscall(SYS_renameat, AT_FDCWD, oldpath, AT_FDCWD, newpath) < 0)
        return fail("renameat");
    if (stat(newpath, &st) < 0 || st.st_size != (off_t)strlen(msg))
        return fail("renameat-stat");
    if (stat(oldpath, &st) == 0)
        return fail("renameat-old-exists");
    if (unlink(newpath) < 0)
        return fail("renameat-unlink");

    if (unlink(path) < 0)
        return fail("unlink");

    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("mmap");
    strcpy((char *)mem, "mmap-ok");
    if (strcmp((char *)mem, "mmap-ok") != 0)
        return fail("mmap-compare");
    if (munmap(mem, 4096) < 0)
        return fail("munmap");

    errno = 0;
    mem = mmap(NULL, (size_t)-1, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem != MAP_FAILED || errno == 0)
        return fail("mmap-error");

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return fail("clock_gettime");

    int pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0)
        _exit(42);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
        return fail("wait-status");

    /* waitid siginfo_t follows the Linux ABI: on 64-bit targets the
     * siginfo union begins at offset 16 (offset 12 on 32-bit targets).
     * pidfd + WNOWAIT checks both that layout and non-consuming semantics. */
    int waitid_gate[2];
    if (pipe(waitid_gate) < 0)
        return fail("waitid-gate-pipe");
    pid = fork();
    if (pid < 0)
        return fail("waitid-fork");
    if (pid == 0) {
        close(waitid_gate[1]);
        char gate_byte;
        if (read(waitid_gate[0], &gate_byte, 1) != 1)
            _exit(38);
        _exit(37);
    }
    close(waitid_gate[0]);
    int child_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (child_pidfd < 0) {
        write(waitid_gate[1], "x", 1);
        close(waitid_gate[1]);
        return fail("pidfd-open");
    }
    if (write(waitid_gate[1], "x", 1) != 1) {
        close(waitid_gate[1]);
        close(child_pidfd);
        return fail("waitid-gate-release");
    }
    close(waitid_gate[1]);
    siginfo_t child_info;
    memset(&child_info, 0xa5, sizeof(child_info));
    if (waitid(P_PIDFD, (id_t)child_pidfd, &child_info,
               WEXITED | WNOWAIT) < 0)
        return fail("waitid-wnowait");
    if (child_info.si_signo != SIGCHLD || child_info.si_code != CLD_EXITED ||
        child_info.si_pid != pid || child_info.si_uid != getuid() ||
        child_info.si_status != 37)
        return fail("waitid-siginfo-layout");
    memset(&child_info, 0, sizeof(child_info));
    if (waitid(P_PIDFD, (id_t)child_pidfd, &child_info, WEXITED) < 0)
        return fail("waitid-reap");
    if (child_info.si_signo != SIGCHLD || child_info.si_pid != pid ||
        child_info.si_uid != getuid() || child_info.si_status != 37)
        return fail("waitid-reap-siginfo");
    close(child_pidfd);
    errno = 0;
    if (waitpid(pid, &status, WNOHANG) != -1 || errno != ECHILD)
        return fail("waitid-echild");

    if (test_thread_group_pidfd_wait())
        return 1;

    if (test_wait_zombie_leader_with_live_member())
        return 1;

    pid = fork();
    if (pid < 0)
        return fail("exec-fork");
    if (pid == 0) {
        char *exec_argv[] = {
            "syscall_smoke", "exec-argv", "sentinel", NULL
        };
        execv("/bin/syscall_smoke", exec_argv);
        _exit(127);
    }
    status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("exec-waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("exec-status");

    /*
     * rt_sigaction's kernel-side struct is musl's struct k_sigaction, which
     * carries sa_restorer between the flags and the mask on every architecture
     * whose signal.h defines SA_RESTORER (aarch64, x86_64, riscv32, arm32,
     * ppc64le) and has no such slot on the others (riscv64, loongarch64).
     * Get the offset wrong in the kernel and the mask is read out of the
     * restorer on the way in, and written where the caller never looks on the
     * way out -- so a disposition set with a mask comes back with an empty
     * one, silently.  Round-trip a non-empty mask and a non-default handler.
     */
    {
        struct sigaction sa, back;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))0;   /* SIG_DFL */
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, SIGUSR1);
        /* oldact is deliberately NULL: passing it makes the kernel report the
         * disposition that was installed *before* this call, which is still the
         * empty default, so it says nothing about the layout.  Reading the
         * disposition back with a separate query is the check. */
        if (sigaction(SIGUSR2, &sa, NULL) < 0)
            return fail("sigaction-set");

        memset(&back, 0, sizeof(back));
        if (sigaction(SIGUSR2, NULL, &back) < 0)
            return fail("sigaction-get");
        if (!sigismember(&back.sa_mask, SIGUSR1))
            return fail("sigaction-mask-roundtrip");
        if (back.sa_handler != (void (*)(int))0)
            return fail("sigaction-handler-roundtrip");

        /* SIG_IGN, to cover the other reserved handler value. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))1;   /* SIG_IGN */
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR2, &sa, NULL) < 0)
            return fail("sigaction-ignore");
        memset(&back, 0, sizeof(back));
        if (sigaction(SIGUSR2, NULL, &back) < 0)
            return fail("sigaction-ignore-get");
        if (back.sa_handler != (void (*)(int))1)
            return fail("sigaction-ignore-roundtrip");
    }

    printf("SYSCALL_SMOKE: PASS\n");
    return 0;
}
