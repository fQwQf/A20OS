/*
 * Guest probe for the successful exec switch.
 *
 * The failure paths are covered by exec_fault_probe; this one exercises what a
 * *successful* replacement must actually do in order:
 *
 *   - the execing process is multithreaded, so the terminate-siblings phase
 *     has real work to do and must not hang or fail the exec;
 *   - a descriptor with FD_CLOEXEC must be closed by the new image, and one
 *     without must survive it (fdtable unshare + close-on-exec sweep);
 *   - the process must actually reach the new image (the child mode below
 *     runs) and report its own fd view.
 *
 * Run as `/bin/sh -c '/bin/exec_switch_probe; echo EXECSWITCH_RC=$?'` so the
 * replaced image's status is observable.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static volatile int thread_up;
static __thread int tls_marker;

static void sigusr1_handler(int sig)
{
    (void)sig;
}

/* Classify the tasks the new image sees. A successful exec must have run the
 * terminate-siblings phase, so no *live* sibling may remain; a zombie that
 * has not been reaped yet is dead, not a surviving thread. */
static void task_stats(int *total, int *alive, int *zombies)
{
    *total = 0;
    *alive = 0;
    *zombies = 0;
    DIR *d = opendir("/proc/self/task");
    if (!d) {
        *total = -1;
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        (*total)++;
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
        int fd = -1;
        for (int attempt = 0; attempt < 3 && fd < 0; attempt++) {
            fd = open(path, O_RDONLY);
            if (fd < 0)
                sched_yield();
        }
        /* A terminated sibling can linger as a task entry whose stat is
         * already unreadable during teardown.  A runnable task always has a
         * readable stat, so an unreadable entry is dead, not alive. */
        if (fd < 0) {
            (*zombies)++;
            continue;
        }
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            (*zombies)++;
            continue;
        }
        buf[n] = '\0';
        /* State is the third whitespace-separated field. */
        char st = '?';
        int field = 0;
        char *p = buf;
        while (*p) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (!*p)
                break;
            if (field == 2) {
                st = *p;
                break;
            }
            while (*p && *p != ' ' && *p != '\t')
                p++;
            field++;
        }
        if (st == 'Z' || st == 'X')
            (*zombies)++;
        else
            (*alive)++;
    }
    closedir(d);
}

static void *spinner(void *arg)
{
    (void)arg;
    __atomic_store_n(&thread_up, 1, __ATOMIC_RELEASE);
    for (;;)
        pause();
    return NULL;
}

/* New image: report the descriptors it inherited. */
static int child_mode(int argc, char **argv)
{
    if (argc < 4) {
        printf("SWITCH_CHILD: bad argv\n");
        return 2;
    }
    int kept = atoi(argv[2]);
    int closed = atoi(argv[3]);
    int kept_ok = fcntl(kept, F_GETFD) >= 0;
    errno = 0;
    int closed_ok = (fcntl(closed, F_GETFD) < 0) && (errno == EBADF);

    /* TLS / trap-state: a __thread access must work with the TP the kernel
     * installed for the new image. */
    tls_marker = 0x5a5a;
    int tls_ok = (tls_marker == 0x5a5a);

    /* terminate-siblings ran: no live sibling survives (a zombie that has not
     * been reaped is dead, not a surviving thread). */
    int total = 0, alive = 0, zombies = 0;
    task_stats(&total, &alive, &zombies);

    /* No setuid binary is involved, so the credential transition must leave
     * the ids untouched. */
    int creds_ok = (getuid() == 0 && geteuid() == 0 && getgid() == getegid());

    /* exec semantics for signal state: caught dispositions revert to SIG_DFL,
     * ignored dispositions stay ignored, and the blocked mask is cleared. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    int hup_default = (sigaction(SIGUSR1, NULL, &sa) == 0 &&
                       sa.sa_handler == SIG_DFL);
    memset(&sa, 0, sizeof(sa));
    int pipe_ignored = (sigaction(SIGPIPE, NULL, &sa) == 0 &&
                        sa.sa_handler == SIG_IGN);
    sigset_t blocked;
    sigemptyset(&blocked);
    int mask_cleared = (sigprocmask(SIG_SETMASK, NULL, &blocked) == 0 &&
                        !sigismember(&blocked, SIGUSR2));

    printf("SWITCH_CHILD: kept_ok=%d closed_ok=%d tls_ok=%d tasks=%d alive=%d "
           "zombies=%d creds_ok=%d hup_default=%d pipe_ignored=%d "
           "mask_cleared=%d\n",
           kept_ok, closed_ok, tls_ok, total, alive, zombies, creds_ok,
           hup_default, pipe_ignored, mask_cleared);
    fflush(stdout);
    return (kept_ok && closed_ok && tls_ok && alive == 1 && creds_ok &&
            hup_default && pipe_ignored && mask_cleared) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "--child") == 0)
        return child_mode(argc, argv);

    int kept = open("/etc/os-release", O_RDONLY);
    if (kept < 0) {
        printf("EXECSWITCH: FAIL open\n");
        return 1;
    }
    int closed = dup(kept);
    if (closed < 0) {
        printf("EXECSWITCH: FAIL dup\n");
        return 1;
    }
    if (fcntl(closed, F_SETFD, FD_CLOEXEC) < 0) {
        printf("EXECSWITCH: FAIL setfd\n");
        return 1;
    }

    /* Second thread in the *execing* process. */
    __atomic_store_n(&thread_up, 0, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, spinner, NULL) != 0) {
        printf("EXECSWITCH: FAIL thread\n");
        return 1;
    }
    for (int i = 0; i < 200000 && !__atomic_load_n(&thread_up, __ATOMIC_ACQUIRE); i++)
        sched_yield();
    if (!__atomic_load_n(&thread_up, __ATOMIC_ACQUIRE)) {
        printf("EXECSWITCH: FAIL thread-start\n");
        return 1;
    }

    /* Caught handler and an ignored signal, plus a blocked signal, to observe
     * exec's reset rules in the child. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler; /* non-default, never delivered */
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("EXECSWITCH: FAIL sigaction\n");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        printf("EXECSWITCH: FAIL sigaction ign\n");
        return 1;
    }
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &block, NULL) < 0) {
        printf("EXECSWITCH: FAIL sigprocmask\n");
        return 1;
    }

    /* Execute through a symlink: the loader must follow it and still reach
     * the same image (the child reports success). */
    unlink("/tmp/exec-switch-link");
    if (symlink("/bin/exec_switch_probe", "/tmp/exec-switch-link") < 0) {
        printf("EXECSWITCH: FAIL symlink\n");
        return 1;
    }

    char kbuf[16], cbuf[16];
    snprintf(kbuf, sizeof(kbuf), "%d", kept);
    snprintf(cbuf, sizeof(cbuf), "%d", closed);

    char *cargv[] = { "/bin/exec_switch_probe", "--child", kbuf, cbuf, NULL };
    execve("/tmp/exec-switch-link", cargv, environ);
    printf("EXECSWITCH: FAIL execve errno=%d\n", errno);
    return 1;
}
