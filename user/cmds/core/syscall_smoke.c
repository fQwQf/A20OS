#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int fail(const char *what)
{
    printf("SYSCALL_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
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

    printf("SYSCALL_SMOKE: PASS\n");
    return 0;
}
