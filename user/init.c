#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime_setup.h"

static void xmkdir(const char *p) { mkdir(p, 0755); }

static void write_file(const char *path, const char *data, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd >= 0) { write(fd, data, strlen(data)); close(fd); }
}

static void append_if_dir(char *buf, size_t cap, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t len = strlen(buf);
        snprintf(buf + len, cap - len, "%s%s", len ? ":" : "", path);
    }
}

static pid_t shell_pid;
static volatile sig_atomic_t got_signal;

static void sig_handler(int sig)
{
    (void)sig;
    got_signal = 1;
    if (shell_pid > 0)
        kill(shell_pid, SIGTERM);
}

static void setup_console(void)
{
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > 2)
        close(fd);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &sa, NULL);
}

static void reap_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

static void do_shutdown(void)
{
    sync();
    printf("[init] shutting down\n");
    reboot(RB_POWER_OFF);
    for (;;) __asm__ volatile("");
}

int main(void)
{
    setsid();
    setup_console();
    setup_signals();

    setup_runtime_links();

    xmkdir("/tmp");
    xmkdir("/tmp/sysinfo");
    write_file("/tmp/sysinfo/model", "A20OS Virtual Machine\n", 0644);

    char path_val[512] = "/bin";
    static const char *path_dirs[] = {
        "/usr/bin", "/test", "/test/glibc", "/test/musl",
#if defined(__loongarch64)
        "/testla/glibc", "/testla/musl", "/testrv/glibc", "/testrv/musl",
#else
        "/testrv/glibc", "/testrv/musl", "/testla/glibc", "/testla/musl",
#endif
        NULL,
    };
    for (const char **p = path_dirs; *p; p++)
        append_if_dir(path_val, sizeof(path_val), *p);

    char ld_val[256] = "";
    static const char *ld_dirs[] = {
        "/test/glibc/lib",
#if defined(__loongarch64)
        "/testla/glibc/lib", "/testrv/glibc/lib",
#else
        "/testrv/glibc/lib", "/testla/glibc/lib",
#endif
        "/glibc/lib", "/usr/lib", NULL,
    };
    for (const char **p = ld_dirs; *p; p++)
        append_if_dir(ld_val, sizeof(ld_val), *p);
    if (!ld_val[0]) strcpy(ld_val, "/lib");

    char path_env[576], ld_env[320];
    snprintf(path_env, sizeof(path_env), "PATH=%s", path_val);
    snprintf(ld_env, sizeof(ld_env), "LD_LIBRARY_PATH=%s", ld_val);

    int contest = (access("/bin/etc/contest-mode", F_OK) == 0);
    char *script = contest ? "/bin/contest.sh" : NULL;
    char *argv[] = {"mksh", script, NULL};
    char *envp[] = {path_env, ld_env, "HOME=/", "SHELL=/bin/mksh", "TERM=vt100", NULL};

    shell_pid = fork();
    if (shell_pid == 0) {
        execve("/bin/mksh", argv, envp);
        perror("execve mksh");
        _exit(127);
    }
    if (shell_pid < 0) {
        perror("fork");
        do_shutdown();
    }

    for (;;) {
        int status;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (got_signal)
                break;
            continue;
        }
        if (w == shell_pid) {
            shell_pid = 0;
            break;
        }
    }

    reap_children();
    do_shutdown();
}
