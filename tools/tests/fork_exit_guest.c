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
#define FILE_PAGES 256u

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

static void check_file_maps(volatile unsigned char *clean,
                            volatile unsigned char *modified)
{
    for (size_t i = 0; i < FILE_PAGES; i++) {
        unsigned char original = (unsigned char)(i + 1);
        unsigned char changed = i < 16 && i % 2 == 0 ? 0xa5 : original;
        if (clean[i * 4096] != original || clean[i * 4096 + 17] != original ||
            modified[i * 4096] != original || modified[i * 4096 + 17] != changed)
            _exit(20);
    }
    if (mprotect((void *)modified, FILE_PAGES * 4096, PROT_READ | PROT_WRITE))
        _exit(21);
    modified[0] = 0xee;
    if (clean[0] != 1) _exit(22);
}

static void file_mapping_inheritance(void)
{
    char name[] = "/tmp/fork-exit-file-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) fail("file create");
    unsigned char page[4096];
    for (unsigned i = 0; i <= FILE_PAGES; i++) {
        memset(page, i, sizeof(page));
        if (write(fd, page, sizeof(page)) != sizeof(page)) fail("file fill");
    }
    const size_t len = FILE_PAGES * 4096;
    volatile unsigned char *clean = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 4096);
    volatile unsigned char *modified = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 4096);
    if (clean == MAP_FAILED || modified == MAP_FAILED) fail("file mmap");
    for (unsigned i = 0; i < FILE_PAGES; i++)
        if (clean[i * 4096] != (unsigned char)(i + 1) ||
            modified[i * 4096] != (unsigned char)(i + 1))
            fail("file initial read");
    if (mprotect((void *)modified, 16 * 4096, PROT_READ | PROT_WRITE))
        fail("file writable protection");
    for (unsigned i = 0; i < 16; i += 2) modified[i * 4096 + 17] = 0xa5;
    if (mprotect((void *)modified, len, PROT_READ)) fail("file read-only protection");
    if (close(fd) || unlink(name)) fail("file detach names");
    for (unsigned i = 0; i < 8; i++) {
        pid_t child = fork();
        if (!child) { check_file_maps(clean, modified); _exit(0); }
        wait_ok(child);
        if (modified[0] != 1) fail("private child COW");
    }
    int gate[2];
    if (pipe(gate)) fail("file gate");
    pid_t child = fork();
    if (!child) {
        close(gate[1]);
        if (read(gate[0], page, 1) != 1) _exit(23);
        check_file_maps(clean, modified);
        _exit(0);
    }
    if (child < 0) fail("file fork");
    close(gate[0]);
    if (munmap((void *)clean, len) || munmap((void *)modified, len))
        fail("parent file unmap");
    if (write(gate[1], "x", 1) != 1) fail("file gate release");
    close(gate[1]); wait_ok(child);
    puts("FORK_EXIT_PROBE: clean file refault, modified read-only COW, orphaned backing PASS");
}

static void targeted_waits(void)
{
    enum { CHILDREN = 32 };
    pid_t children[CHILDREN];
    int gate[2], status;
    if (pipe(gate)) fail("wait gate");
    for (int i = 0; i < CHILDREN; i++) {
        children[i] = fork();
        if (!children[i]) {
            char byte;
            close(gate[1]);
            if (read(gate[0], &byte, 1) != 1) _exit(100);
            _exit(i);
        }
        if (children[i] < 0) fail("wait fork");
    }
    close(gate[0]);
    if (waitpid(children[0], &status, WNOHANG) != 0) fail("wait WNOHANG");
    char release[CHILDREN]; memset(release, 1, sizeof(release));
    if (write(gate[1], release, sizeof(release)) != sizeof(release))
        fail("wait release");
    close(gate[1]);
    siginfo_t info;
    for (int i = 0; i < 2; i++) {
        if (waitid(P_PID, children[CHILDREN - 1], &info, WEXITED | WNOWAIT) ||
            info.si_pid != children[CHILDREN - 1] || info.si_status != CHILDREN - 1)
            fail("targeted WNOWAIT");
    }
    for (int i = CHILDREN - 1; i >= 0; i--)
        if (waitpid(children[i], &status, 0) != children[i] ||
            !WIFEXITED(status) || WEXITSTATUS(status) != i)
            fail("targeted reverse wait");
    if (waitpid(-1, &status, WNOHANG) != -1 || errno != ECHILD)
        fail("wait final ECHILD");
    puts("FORK_EXIT_PROBE: 32 concurrent children, WNOHANG, WNOWAIT, reverse PID waits PASS");
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
    file_mapping_inheritance();
    targeted_waits();
    puts("SHARED_EXIT_PROBE: PASS rounds=128 shared_visibility=1 mprotect=1 "
         "frame_reuse=1 untouched_backing=1");
    return 0;
}
