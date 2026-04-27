#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include "runtime_setup.h"

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void append_existing_dir(char *buf, size_t buf_size, const char *path)
{
    size_t len;
    int n;

    if (!dir_exists(path))
        return;

    len = strlen(buf);
    n = snprintf(buf + len, buf_size - len, "%s%s", len == 0 ? "" : ":", path);
    if (n < 0 || (size_t)n >= buf_size - len)
        buf[buf_size - 1] = '\0';
}

static void build_glibc_shell_env(char *path_env, size_t path_env_size,
                                  char *ld_env, size_t ld_env_size)
{
    char path_value[512];
    char ld_value[512];

    snprintf(path_value, sizeof(path_value), "/bin");
    append_existing_dir(path_value, sizeof(path_value), "/test");
    append_existing_dir(path_value, sizeof(path_value), "/test/glibc");
    append_existing_dir(path_value, sizeof(path_value), "/testrv/glibc");
    append_existing_dir(path_value, sizeof(path_value), "/testla/glibc");

    ld_value[0] = '\0';
    append_existing_dir(ld_value, sizeof(ld_value), "/test/glibc/lib");
    append_existing_dir(ld_value, sizeof(ld_value), "/testrv/glibc/lib");
    append_existing_dir(ld_value, sizeof(ld_value), "/testla/glibc/lib");
    append_existing_dir(ld_value, sizeof(ld_value), "/glibc/lib");
    if (ld_value[0] == '\0')
        snprintf(ld_value, sizeof(ld_value), "/lib");

    snprintf(path_env, path_env_size, "PATH=%s", path_value);
    snprintf(ld_env, ld_env_size, "LD_LIBRARY_PATH=%s", ld_value);
}

static int test_file_io(void)
{
    const char *path = "/testrv/testfile.txt";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        printf("FAIL: open write: %d\n", errno);
        return 1;
    }
    const char *data = "Hello from A20OS test!\n";
    if (write(fd, data, strlen(data)) != (int)strlen(data))
    {
        printf("FAIL: write\n");
        close(fd);
        return 1;
    }
    close(fd);

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        printf("FAIL: open read\n");
        return 1;
    }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0)
    {
        printf("FAIL: read\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, data) != 0)
    {
        printf("FAIL: data mismatch: '%s' vs '%s'\n", buf, data);
        return 1;
    }

    if (unlink(path) < 0)
    {
        printf("FAIL: unlink\n");
        return 1;
    }
    printf("PASS: file I/O\n");
    return 0;
}

static int test_dir_ops(void)
{
    const char *dir = "/testrv/testdir";
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
    {
        printf("FAIL: mkdir: %d\n", errno);
        return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/sub.txt", dir);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
    {
        printf("FAIL: open in dir\n");
        return 1;
    }
    close(fd);
    if (unlink(path) < 0)
    {
        printf("FAIL: unlink in dir\n");
        return 1;
    }
    if (rmdir(dir) < 0)
    {
        printf("FAIL: rmdir: %d\n", errno);
        return 1;
    }
    printf("PASS: directory ops\n");
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    setup_runtime_links();

    int pid = fork();
    if (pid < 0)
    {
        printf("[INIT] fork failed, shutting down.\n");
        syscall(SYS_reboot, 0);
    }
    if (pid == 0)
    {
        char path_env[576];
        char ld_env[576];
        build_glibc_shell_env(path_env, sizeof(path_env), ld_env, sizeof(ld_env));
        char *sh_argv[] = {"mksh", NULL};
        char *envp[] = {
            path_env,
            ld_env,
            "HOME=/",
            NULL,
        };
        execve("/bin/mksh", sh_argv, envp);
        printf("[INIT] execve failed.\n");
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        printf("[INIT] Shell exited cleanly. Powering off.\n");
        syscall(SYS_reboot, 0);
    }

    printf("[INIT] Shell exited abnormally (status=%d). Powering off.\n", status);
    while (1)
    {
        syscall(SYS_reboot, 0);
    }
    return 0;
}
