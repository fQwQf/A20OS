/* Build with musl and run on Linux/A20 to check fork/exit ownership. */
#define _GNU_SOURCE
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#define SZ (2u * 1024 * 1024)

static void fail(const char *message)
{
    perror(message);
    exit(1);
}

static void wait_ok(pid_t child)
{
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status))
        fail("child/wait");
}

static void high_fd_inheritance(void)
{
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) fail("getrlimit");
    if (limit.rlim_cur < 4096) {
        limit.rlim_cur = 4096;
        if (limit.rlim_max < limit.rlim_cur) limit.rlim_max = limit.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &limit)) fail("setrlimit");
    }
    int low = open("/dev/null", O_RDWR);
    if (low < 0) fail("open null");
    const int targets[] = {63, 64, 127, 128, 1023, 4095};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        int high = fcntl(low, F_DUPFD_CLOEXEC, targets[i]);
        if (high != targets[i]) fail("high fd allocation");
        for (int round = 0; round < 8; round++) {
            pid_t child = fork();
            if (child == 0) {
                if (!(fcntl(high, F_GETFD) & FD_CLOEXEC)) _exit(10);
                if (close(high) || fcntl(high, F_GETFD) != -1 || errno != EBADF)
                    _exit(11);
                if (dup2(low, high) != high || fcntl(high, F_GETFD) != 0)
                    _exit(12);
                _exit(0);
            }
            wait_ok(child);
            if (fcntl(high, F_GETFD) != FD_CLOEXEC) fail("parent fd ownership");
        }
        if (close(high)) fail("close high fd");
    }
    if (close(low)) fail("close low fd");
    puts("FORK_EXIT_PROBE: fd boundaries and fork ownership PASS");
}

int main(void)
{
    volatile unsigned char *shared = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        fail("shared mmap");
    for (size_t i = 0; i < SZ; i += 4096)
        shared[i] = (unsigned char)(i / 4096 % 251 + 1);
    for (unsigned round = 0; round < 128; round++) {
        pid_t child = fork();
        if (!child) {
            shared[17] = (unsigned char)round;
            if (round % 2 == 0 && mprotect((void *)shared, SZ, PROT_READ))
                _exit(2);
            _exit(0);
        }
        wait_ok(child);
        if (shared[17] != (unsigned char)round)
            fail("shared visibility");
        /* Force reuse of any frames that exit incorrectly freed while the
         * parent still owns its mapping. Check data, not just ref counters. */
        unsigned char *poison = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (poison == MAP_FAILED)
            fail("poison mmap");
        memset(poison, 0xa5, SZ);
        for (size_t i = 0; i < SZ; i += 4096)
            if (shared[i] != (unsigned char)(i / 4096 % 251 + 1))
                fail("shared frame prematurely released");
        if (munmap(poison, SZ))
            fail("poison munmap");
    }
    if (munmap((void *)shared, SZ))
        fail("shared munmap");

    size_t sparse = 16u * 1024 * 1024;
    shared = mmap(NULL, sparse, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        fail("sparse mmap");
    shared[0] = 23;
    pid_t child = fork();
    if (!child) {
        if (shared[0] != 23)
            _exit(3);
        shared[sparse - 4096] = 67;
        _exit(0);
    }
    wait_ok(child);
    if (shared[0] != 23 || shared[sparse - 4096] != 67)
        fail("untouched shared backing");
    if (munmap((void *)shared, sparse))
        fail("sparse munmap");
    high_fd_inheritance();
    puts("SHARED_EXIT_PROBE: PASS rounds=128 shared_visibility=1 mprotect=1 "
         "frame_reuse=1 untouched_backing=1");
    return 0;
}
