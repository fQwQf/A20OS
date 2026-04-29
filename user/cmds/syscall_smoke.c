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

int main(void)
{
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

    printf("SYSCALL_SMOKE: PASS\n");
    return 0;
}
