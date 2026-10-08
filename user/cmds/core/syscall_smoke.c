#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <pthread.h>

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
    pid = fork();
    if (pid < 0)
        return fail("waitid-fork");
    if (pid == 0)
        _exit(37);
    int child_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (child_pidfd < 0)
        return fail("pidfd-open");
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
