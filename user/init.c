#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

int main(void)
{
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

    char *script = (access("/bin/etc/contest-mode", F_OK) == 0) ? "/bin/contest.sh" : NULL;
    char *argv[] = {"mksh", script, NULL};
    char *envp[] = {path_env, ld_env, "HOME=/", "SHELL=/bin/mksh", "TERM=vt100", NULL};

    pid_t pid = fork();
    if (pid == 0) {
        execve("/bin/mksh", argv, envp);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    for (;;) syscall(SYS_reboot, 0);
}
