/* Build with musl and run on Linux/A20 to check fork/exit ownership. */
#define _GNU_SOURCE
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/file.h>
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

static void child_signal_handler(int sig) { (void)sig; }

static void signal_only_waits(void)
{
    struct sigaction action = {0}, previous;
    action.sa_handler = child_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, &previous)) fail("SIGCHLD action");
    sigset_t set, old;
    sigemptyset(&set); sigaddset(&set, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &set, &old)) fail("block SIGCHLD");
    struct timespec zero = {0}, limit = {2, 0};
    while (sigtimedwait(&set, NULL, &zero) == SIGCHLD) {}
    for (int i = 0; i < 512; i++) {
        pid_t child = fork();
        if (!child) _exit(i % 127);
        if (child < 0) fail("signal-only fork");
        siginfo_t info;
        int sig = sigtimedwait(&set, &info, &limit);
        if (sig != SIGCHLD || info.si_pid != child) {
            fprintf(stderr, "signal round=%d sig=%d pid=%d expected=%d\n",
                    i, sig, sig > 0 ? info.si_pid : -1, child);
            fail("signal-only notification");
        }
        int status;
        if (waitpid(child, &status, WNOHANG) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != i % 127)
            fail("SIGCHLD precedes reap readiness");
    }
    if (sigprocmask(SIG_SETMASK, &old, NULL)) fail("restore signals");
    if (sigaction(SIGCHLD, &previous, NULL)) fail("restore SIGCHLD action");
    puts("SIGNAL_WAIT_PROBE: PASS sigchld_wnohang=512");
}

static void pidfd_ready_waits(void)
{
    for (int i = 0; i < 128; i++) {
        int gate[2];
        if (pipe(gate)) fail("pidfd gate");
        pid_t child = fork();
        if (!child) {
            char byte; close(gate[1]);
            _exit(read(gate[0], &byte, 1) == 1 ? 0 : 3);
        }
        if (child < 0) fail("pidfd fork");
        int fd = syscall(SYS_pidfd_open, child, 0);
        if (fd < 0) fail("pidfd_open");
        close(gate[0]);
        if (write(gate[1], "x", 1) != 1) fail("pidfd release");
        close(gate[1]);
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 2000) != 1 || !(p.revents & POLLIN)) fail("pidfd poll");
        int status;
        if (waitpid(child, &status, WNOHANG) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status)) fail("pidfd precedes reap readiness");
        close(fd);
    }
    puts("PIDFD_WAIT_PROBE: PASS poll_then_wnohang=128");
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

/* Align two complete L0 tables, warm their fork proofs, then invalidate them
 * through protection changes and private copies. A stale proof loses data. */
static void file_fork_proof_invalidation(void)
{
    const size_t len = 2 * SZ, reserve_len = len + SZ;
    char name[] = "/tmp/fork-file-proof-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0 || unlink(name) || ftruncate(fd, len)) fail("proof file");
    void *reserve = mmap(NULL, reserve_len, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) fail("proof reserve");
    uintptr_t aligned = ((uintptr_t)reserve + SZ - 1) & ~(uintptr_t)(SZ - 1);
    volatile unsigned char *map = mmap((void *)aligned, len, PROT_READ,
                                       MAP_PRIVATE | MAP_FIXED, fd, 0);
    if (map == MAP_FAILED) fail("proof map");
    for (size_t i = 0; i < len; i += 4096)
        if (map[i]) fail("proof initial read");
    for (unsigned round = 0; round < 4; round++) {
        pid_t child = fork();
        if (!child) { if (map[0] || map[len - 4096]) _exit(40); _exit(0); }
        wait_ok(child);
    }
    if (mprotect((void *)map, len, PROT_READ | PROT_WRITE)) fail("proof writable");
    for (size_t i = 0; i < len; i += 4096)
        map[i] = (unsigned char)(i / 4096 % 251 + 1);
    if (mprotect((void *)map, len, PROT_READ)) fail("proof readonly");
    if (close(fd)) fail("proof close");
    for (unsigned round = 0; round < 4; round++) {
        pid_t child = fork();
        if (!child) {
            for (size_t i = 0; i < len; i += 4096)
                if (map[i] != (unsigned char)(i / 4096 % 251 + 1)) _exit(41);
            if (mprotect((void *)map, len, PROT_READ | PROT_WRITE)) _exit(42);
            map[SZ] = 0xee;
            _exit(0);
        }
        wait_ok(child);
        if (map[SZ] != (unsigned char)(SZ / 4096 % 251 + 1)) fail("proof child COW");
    }
    if (munmap(reserve, reserve_len)) fail("proof release");
    puts("FORK_EXIT_PROBE: full file table proof, mprotect, private copy, nested COW PASS");
}

/* Below the lazy-VMO threshold: exercise borrowed batch clone/teardown. */
static void small_shared_batch_lifetime(void)
{
    const size_t len=64*4096;
    volatile unsigned char *map=mmap(NULL,len,PROT_READ|PROT_WRITE,
                                     MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(map==MAP_FAILED)fail("small shared map");
    for(size_t i=0;i<len;i+=4096)map[i]=0;
    for(unsigned round=1;round<=32;round++) {
        pid_t child=fork();
        if(!child){for(size_t i=0;i<len;i+=4096){if(map[i]!=round-1)_exit(56);map[i]=round;}_exit(0);}
        wait_ok(child);
        for(size_t i=0;i<len;i+=4096)if(map[i]!=round)fail("small shared contents");
    }
    int gate[2];if(pipe(gate))fail("small shared gate");
    pid_t child=fork();
    if(!child){char token;if(read(gate[0],&token,1)!=1)_exit(57);for(size_t i=0;i<len;i+=4096)if(map[i]!=32)_exit(58);_exit(0);}
    if(child<0||munmap((void *)map,len))fail("small shared parent release");
    void *poison=mmap(NULL,4*len,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(poison==MAP_FAILED)fail("small shared poison");memset(poison,0xa5,4*len);
    if(write(gate[1],"x",1)!=1)fail("small shared wake");wait_ok(child);
    munmap(poison,4*len);close(gate[0]);close(gate[1]);
    puts("FORK_EXIT_PROBE: small VMO batch, repeated child writes, last mapper, frame reuse PASS");
}

static void file_lock_lifetime(void)
{
    char name[] = "/tmp/fork-lock-life-XXXXXX";
    int fd = mkstemp(name);
    int other = fd < 0 ? -1 : open(name, O_RDWR);
    if (fd < 0 || other < 0 || unlink(name)) fail("lock file");
    struct flock lock = { .l_type=F_WRLCK, .l_whence=SEEK_SET, .l_start=0, .l_len=100 };
    if (fcntl(fd, F_SETLK, &lock)) fail("parent lock");
    for (int round=0; round<2; round++) {
        pid_t child=fork();
        if (!child) {
            errno=0;
            if (fcntl(other,F_SETLK,&lock)!=-1 || (errno!=EAGAIN && errno!=EACCES)) _exit(50);
            _exit(0);
        }
        wait_ok(child); /* Child close must not release the parent's locks. */
    }
    lock.l_type=F_UNLCK;lock.l_start=40;lock.l_len=20;
    if (fcntl(fd,F_SETLK,&lock)) fail("split lock");
    pid_t child=fork();
    if (!child) {
        lock.l_type=F_WRLCK;
        if (fcntl(other,F_SETLK,&lock)) _exit(51);
        lock.l_start=0;lock.l_len=10;
        if (fcntl(other,F_SETLK,&lock)!=-1) _exit(52);
        _exit(0);
    }
    wait_ok(child);
    int alias=dup(fd);
    if(alias<0 || close(alias)) fail("POSIX close release");
    child=fork();
    if(!child) {lock.l_type=F_WRLCK;lock.l_start=0;lock.l_len=100;if(fcntl(other,F_SETLK,&lock))_exit(53);_exit(0);}
    wait_ok(child);
    lock.l_type=F_WRLCK;lock.l_start=0;lock.l_len=100;
    if(fcntl(fd,F_OFD_SETLK,&lock))fail("OFD lock");
    child=fork();
    if(!child){if(fcntl(other,F_OFD_SETLK,&lock)!=-1)_exit(54);_exit(0);}
    wait_ok(child);
    if(fcntl(other,F_OFD_SETLK,&lock)!=-1)fail("OFD child exit ownership");
    lock.l_type=F_UNLCK;if(fcntl(fd,F_OFD_SETLK,&lock))fail("OFD unlock");
    if(flock(fd,LOCK_EX|LOCK_NB))fail("flock acquire");
    child=fork();if(!child){if(flock(other,LOCK_EX|LOCK_NB)!=-1)_exit(55);_exit(0);}wait_ok(child);
    if(flock(other,LOCK_EX|LOCK_NB)!=-1)fail("flock child exit ownership");
    if(flock(fd,LOCK_UN)||flock(other,LOCK_EX|LOCK_NB))fail("flock release");
    close(other);close(fd);
    puts("FORK_EXIT_PROBE: POSIX split/close, OFD/flock fork/exit lifetime PASS");
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

static void sparse_shared_semantics(void)
{
    const size_t len = 256u * 1024 * 1024, far = len - 4096;
    volatile unsigned char *map = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) fail("sparse shared reserve");
    map[0] = 37;
    /* Split the VMA before fork; the tail must retain its backing offset. */
    if (mprotect((void *)map, 4096, PROT_READ)) fail("shared split");
    int gate[2]; pid_t children[8];
    if (pipe(gate)) fail("shared gate");
    for (unsigned i = 0; i < 8; i++) {
        children[i] = fork();
        if (!children[i]) {
            char byte; close(gate[1]);
            if (read(gate[0], &byte, 1) != 1 || map[0] != 37) _exit(30);
            /* All children first-touch one canonical page concurrently. */
            map[far + i] = (unsigned char)(i + 10);
            map[(i + 1) * 16u * 1024 * 1024] = (unsigned char)(i + 20);
            _exit(0);
        }
        if (children[i] < 0) fail("shared fork");
    }
    close(gate[0]);
    if (write(gate[1], "12345678", 8) != 8) fail("shared release");
    close(gate[1]);
    for (unsigned i = 0; i < 8; i++) wait_ok(children[i]);
    for (unsigned i = 0; i < 8; i++)
        if (map[far + i] != i + 10 || map[(i + 1) * 16u * 1024 * 1024] != i + 20)
            fail("shared canonical first touch");

    unsigned char *alias = mremap((void *)(map + far), 0, 4096, MREMAP_MAYMOVE);
    if (alias == MAP_FAILED) fail("shared alias");
    alias[123] = 91;
    if (map[far + 123] != 91) fail("shared alias offset");
    unsigned char resident = 0;
    if (madvise(alias, 4096, MADV_DONTNEED) ||
        mincore(alias, 4096, &resident) || !(resident & 1) || alias[123] != 91)
        fail("shared DONTNEED backing");
    if (munmap((void *)map, len) || alias[123] != 91)
        fail("shared alias survives unmap");
    if (munmap(alias, 4096)) fail("shared alias release");

    const size_t moved_len = 2u * 1024 * 1024;
    map = mmap(NULL, moved_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *target = mmap(NULL, moved_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED || target == MAP_FAILED) fail("shared move reserve");
    map[0] = 71; map[moved_len - 4096] = 72;
    map = mremap((void *)map, moved_len, moved_len, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    if (map != target || map[0] != 71 || map[moved_len - 4096] != 72)
        fail("shared move content");
    if (pipe(gate)) fail("orphan shared gate");
    pid_t child = fork();
    if (!child) {
        char byte; close(gate[1]);
        if (read(gate[0], &byte, 1) != 1 || map[0] != 71 || map[moved_len - 4096] != 72)
            _exit(31);
        if (map[4096] != 0) _exit(32);
        map[4096] = 73;
        _exit(0);
    }
    if (child < 0) fail("orphan shared fork");
    close(gate[0]);
    if (munmap((void *)map, moved_len)) fail("orphan shared parent unmap");
    if (write(gate[1], "x", 1) != 1) fail("orphan shared release");
    close(gate[1]); wait_ok(child);

    map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) fail("shared grow reserve");
    map[0] = 81;
    map = mremap((void *)map, 4096, 8192, MREMAP_MAYMOVE);
    if (map == MAP_FAILED || map[0] != 81) fail("shared grow VMA");
    child = fork();
    if (!child) { map[4096] = 1; _exit(33); }
    int status;
    if (child < 0 || waitpid(child, &status, 0) != child ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS)
        fail("shared grow SIGBUS");
    if (munmap((void *)map, 8192)) fail("shared grow release");
    puts("FORK_EXIT_PROBE: sparse shared first-touch, split, alias, move, DONTNEED, last mapper, SIGBUS PASS");
}

/* Resident lookahead must not send a 64-page mixed window to the bounded
 * fill transaction: the first 16 pages can be cold while its tail is hot. */
static void file_fault_cold_prefix(void)
{
    char name[] = "/tmp/a20-cold-prefix-XXXXXX";
    int fd = mkstemp(name);
    size_t len = 1024 * 1024;
    if (fd < 0 || unlink(name) || ftruncate(fd, len)) fail("cold-prefix file");
    unsigned char *buf = malloc(64 * 4096);
    if (!buf || pread(fd, buf, 64 * 4096, 32 * 4096) != 64 * 4096)
        fail("cold-prefix warm tail");
    volatile unsigned char *p = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) fail("cold-prefix mmap");
    if (p[0] || p[15 * 4096] || p[16 * 4096] || p[79 * 4096])
        fail("cold-prefix contents");
    if (munmap((void *)p, len) || close(fd)) fail("cold-prefix cleanup");
    free(buf);
    puts("FILE_COLD_PREFIX: PASS bounded fill with resident tail");
}

int main(void)
{
    signal_only_waits();
    pidfd_ready_waits();
    volatile unsigned char *shared = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        fail("shared mmap");
    for (size_t i = 0; i < SZ; i += 4096)
        shared[i] = (unsigned char)(i / 4096 % 251 + 1);
    for (unsigned round = 0; round < 128; round++) {
        pid_t child = fork();
        if (!child) {
            unsigned char resident = 0;
            if (mincore((void *)shared, 4096, &resident) || !(resident & 1))
                _exit(8);
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
    file_lock_lifetime();
    small_shared_batch_lifetime();
    file_fork_proof_invalidation();
    file_fault_cold_prefix();
    targeted_waits();
    sparse_shared_semantics();
    puts("SHARED_EXIT_PROBE: PASS rounds=128 shared_visibility=1 mprotect=1 "
         "frame_reuse=1 untouched_backing=1");
    return 0;
}
