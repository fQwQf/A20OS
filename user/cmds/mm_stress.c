#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int shared_file_writeback(void)
{
    const char *path = "/tmp/mm_stress_shared";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("shared-open");

    char initial[4096];
    memset(initial, 'A', sizeof(initial));
    if (write(fd, initial, sizeof(initial)) != (ssize_t)sizeof(initial)) {
        close(fd);
        unlink(path);
        return fail("shared-write-initial");
    }

    char *mem = mmap(NULL, sizeof(initial), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("shared-mmap");
    }

    for (size_t i = 0; i < sizeof(initial); i++)
        mem[i] = (char)('0' + (i % 10));

    if (msync(mem, sizeof(initial), MS_SYNC) < 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-msync");
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-lseek");
    }

    char readback[4096];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(readback)) {
        ssize_t n = read(fd, readback + total, sizeof(readback) - total);
        if (n < 0) {
            munmap(mem, sizeof(initial));
            close(fd);
            unlink(path);
            return fail("shared-read");
        }
        if (n == 0)
            break;
        total += n;
    }
    if (total != (ssize_t)sizeof(readback) ||
        memcmp(mem, readback, sizeof(readback)) != 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-compare");
    }

    if (munmap(mem, sizeof(initial)) < 0) {
        close(fd);
        unlink(path);
        return fail("shared-munmap");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int anonymous_fault_and_unmap(void)
{
    char *mem = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("anon-mmap");

    for (int i = 0; i < 8192; i += 512)
        mem[i] = (char)(i / 512 + 1);
    for (int i = 0; i < 8192; i += 512) {
        if (mem[i] != (char)(i / 512 + 1))
            return fail("anon-compare");
    }
    if (munmap(mem, 8192) < 0)
        return fail("anon-munmap");
    return 0;
}

static int fixed_noreplace_conflict(void)
{
    char *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("fixed-base-mmap");

    errno = 0;
    void *again = mmap(mem, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (again != MAP_FAILED) {
        munmap(again, 4096);
        munmap(mem, 4096);
        return fail("fixed-noreplace-overlap");
    }
    if (errno != EEXIST) {
        munmap(mem, 4096);
        return fail("fixed-noreplace-errno");
    }
    if (munmap(mem, 4096) < 0)
        return fail("fixed-munmap");
    return 0;
}

static int fork_cow_and_exit(void)
{
    char *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("cow-mmap");
    strcpy(mem, "parent-value");

    pid_t pid = fork();
    if (pid < 0)
        return fail("cow-fork");
    if (pid == 0) {
        if (strcmp(mem, "parent-value") != 0)
            _exit(2);
        strcpy(mem, "child-value");
        _exit(strcmp(mem, "child-value") == 0 ? 0 : 3);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("cow-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("cow-child-status");
    if (strcmp(mem, "parent-value") != 0)
        return fail("cow-parent-value");
    if (munmap(mem, 4096) < 0)
        return fail("cow-munmap");
    return 0;
}

int main(void)
{
    printf("MM_STRESS: start\n");
    if (anonymous_fault_and_unmap() != 0)
        return 1;
    if (fixed_noreplace_conflict() != 0)
        return 1;
    if (fork_cow_and_exit() != 0)
        return 1;
    if (shared_file_writeback() != 0)
        return 1;
    printf("MM_STRESS: PASS\n");
    return 0;
}
