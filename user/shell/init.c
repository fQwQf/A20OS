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

static int test_pipe(void)
{
    int p[2];
    if (pipe(p) < 0)
    {
        printf("FAIL: pipe\n");
        return 1;
    }
    const char *msg = "pipe test";
    if (write(p[1], msg, strlen(msg)) != (int)strlen(msg))
    {
        printf("FAIL: pipe write\n");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    close(p[1]);
    char buf[256];
    int n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    if (n < 0)
    {
        printf("FAIL: pipe read\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0)
    {
        printf("FAIL: pipe data mismatch\n");
        return 1;
    }
    printf("PASS: pipe\n");
    return 0;
}

static int test_mmap(void)
{
    size_t len = 4096 * 4;
    void *addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED)
    {
        printf("FAIL: mmap\n");
        return 1;
    }
    volatile char *p = (volatile char *)addr;
    for (size_t i = 0; i < len; i++)
        p[i] = (char)(i & 0xFF);
    for (size_t i = 0; i < len; i++)
    {
        if (p[i] != (char)(i & 0xFF))
        {
            printf("FAIL: mmap data mismatch at %zu\n", i);
            munmap(addr, len);
            return 1;
        }
    }
    if (munmap(addr, len) < 0)
    {
        printf("FAIL: munmap\n");
        return 1;
    }
    printf("PASS: mmap/munmap\n");
    return 0;
}

static int test_brk(void)
{
    void *orig = sbrk(0);
    if (orig == (void *)-1)
    {
        printf("FAIL: sbrk(0)\n");
        return 1;
    }
    void *new_brk = (char *)orig + 8192;
    if (brk(new_brk) < 0)
    {
        printf("FAIL: brk\n");
        return 1;
    }
    volatile char *p = (volatile char *)orig;
    for (int i = 0; i < 4096; i++)
        p[i] = (char)i;
    for (int i = 0; i < 4096; i++)
    {
        if (p[i] != (char)i)
        {
            printf("FAIL: brk data mismatch\n");
            return 1;
        }
    }
    if (brk(orig) < 0)
    {
        printf("FAIL: brk restore\n");
        return 1;
    }
    printf("PASS: brk\n");
    return 0;
}

static int test_fork_exec(void)
{
    int pid = fork();
    if (pid < 0)
    {
        printf("FAIL: fork\n");
        return 1;
    }
    if (pid == 0)
    {
        char *argv[] = {"echo", "fork_exec_ok", NULL};
        execve("/bin/echo", argv, NULL);
        _exit(1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        printf("FAIL: waitpid\n");
        return 1;
    }
    if (status != 0)
    {
        printf("FAIL: child exit status %d\n", status);
        return 1;
    }
    printf("PASS: fork/exec\n");
    return 0;
}

static int test_fork_pipe(void)
{
    int p[2];
    if (pipe(p) < 0)
    {
        printf("FAIL: pipe in fork_pipe\n");
        return 1;
    }
    int pid = fork();
    if (pid < 0)
    {
        printf("FAIL: fork in fork_pipe\n");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    if (pid == 0)
    {
        close(p[0]);
        const char *msg = "child_to_parent";
        write(p[1], msg, strlen(msg));
        close(p[1]);
        _exit(0);
    }
    close(p[1]);
    char buf[256];
    int n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    waitpid(pid, NULL, 0);
    if (n < 0)
    {
        printf("FAIL: read in fork_pipe\n");
        return 1;
    }
    buf[n] = '\0';
    if (strcmp(buf, "child_to_parent") != 0)
    {
        printf("FAIL: fork_pipe data mismatch: '%s'\n", buf);
        return 1;
    }
    printf("PASS: fork+pipe\n");
    return 0;
}

static int test_dup(void)
{
    int fd = open("/testrv/testdup.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        printf("FAIL: open dup\n");
        return 1;
    }
    int fd2 = dup(fd);
    if (fd2 < 0)
    {
        printf("FAIL: dup\n");
        close(fd);
        return 1;
    }
    const char *msg = "dup_test";
    if (write(fd2, msg, strlen(msg)) != (int)strlen(msg))
    {
        printf("FAIL: write dup\n");
        close(fd);
        close(fd2);
        return 1;
    }
    close(fd2);
    close(fd);
    fd = open("/testrv/testdup.txt", O_RDONLY, 0);
    if (fd < 0)
    {
        printf("FAIL: open dup verify\n");
        return 1;
    }
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0)
    {
        printf("FAIL: dup data mismatch\n");
        return 1;
    }
    unlink("/testrv/testdup.txt");
    printf("PASS: dup\n");
    return 0;
}

static int test_stress(void)
{
    for (int round = 0; round < 10; round++)
    {
        int pid = fork();
        if (pid < 0)
        {
            printf("FAIL: stress fork round %d\n", round);
            return 1;
        }
        if (pid == 0)
        {
            char cmd[256];
            char path_env[576];
            char ld_env[576];
            snprintf(cmd, sizeof(cmd),
                     "i=1; while [ $i -le %d ]; do mksh -c 'echo 1' & i=$((i+1)); done; wait",
                     10);
            build_glibc_shell_env(path_env, sizeof(path_env), ld_env, sizeof(ld_env));
            char *mksh_argv[] = {"mksh", "-c", cmd, NULL};
            char *envp[] = {
                path_env,
                ld_env,
                "HOME=/",
                NULL,
            };
            execve("/bin/mksh", mksh_argv, envp);
            _exit(1);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (status != 0)
        {
            printf("FAIL: stress round %d status=%d\n", round, status);
            return 1;
        }
    }
    printf("PASS: fork/exec stress\n");
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
