/* OOM stress: drive the cgroup memory-limit OOM path end to end and assert
 * safe-kill semantics plus observability surfaces.
 *
 * Scenario: mount a cgroup2 hierarchy, create a child group with a small
 * memory.max, move a forked child into it, and have the child fault in far
 * more anonymous memory than the limit.  Expected kernel behavior:
 *   - demand faults beyond the limit fail charging and trigger cg OOM kill
 *   - the victim is the worst-scoring task inside that group (the child)
 *   - the child dies with SIGKILL; parent and system keep running
 *   - memory.events reports oom_kill >= 1 after teardown
 * The test then proves system liveness by touching fresh memory and doing
 * file I/O from the surviving parent.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_umount2
#define SYS_umount2 39
#endif

#define CG_DIR      "/tmp/oom_cg"
#define CG_GROUP    CG_DIR "/victim"
#define LIMIT_BYTES (32UL * 1024 * 1024)

static int fail(const char *what)
{
    printf("OOM_STRESS: FAIL at %s (errno=%d)\n", what, errno);
    return 1;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok ? 0 : -1;
}

/* Read a small procfs/cgroupfs file entirely; returns 0 on success. */
static int read_text(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static long parse_key_value(const char *text, const char *key)
{
    const char *p = strstr(text, key);
    if (!p)
        return LONG_MIN;
    p += strlen(key);
    while (*p == ' ' || *p == '\t')
        p++;
    return strtol(p, NULL, 10);
}

static int cleanup_cgroup(void)
{
    syscall(SYS_umount2, CG_DIR, 0);
    rmdir(CG_GROUP);
    rmdir(CG_DIR);
    return 0;
}

/* Child body: join the limited group, then touch 2x the limit of anon
 * memory.  Exit codes distinguish infrastructure failures (43/44) from
 * "kernel never enforced the limit" (45). */
static void run_leaker(void)
{
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
    if (write_text(CG_GROUP "/cgroup.procs", pidbuf) != 0)
        _exit(43);

    size_t len = LIMIT_BYTES * 2;
    volatile char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        _exit(44);
    for (size_t off = 0; off < len; off += 4096)
        p[off] = 1;
    _exit(45);
}

static int cgroup_oom_kill(void)
{
    mkdir(CG_DIR, 0755);
    if (syscall(SYS_mount, "none", CG_DIR, "cgroup2", 0, "") < 0) {
        cleanup_cgroup();
        return fail("oom-cgroup2-mount");
    }
    /* The group must be created inside the already-mounted cgroupfs;
     * a pre-existing directory would be shadowed by the mount. */
    if (mkdir(CG_GROUP, 0755) < 0 && errno != EEXIST) {
        cleanup_cgroup();
        return fail("oom-mkdir-group");
    }
    if (write_text(CG_GROUP "/memory.max", "33554432\n") != 0) {
        cleanup_cgroup();
        return fail("oom-set-memory.max");
    }

    char buf[512];
    if (read_text(CG_GROUP "/memory.max", buf, sizeof(buf)) != 0 ||
        strtol(buf, NULL, 10) != (long)LIMIT_BYTES) {
        cleanup_cgroup();
        return fail("oom-memory.max-readback");
    }

    pid_t pid = fork();
    if (pid < 0) {
        cleanup_cgroup();
        return fail("oom-fork");
    }
    if (pid == 0)
        run_leaker();

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        cleanup_cgroup();
        return fail("oom-wait");
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        printf("OOM_STRESS: child exit status=%d\n", status);
        cleanup_cgroup();
        return fail("oom-child-not-sigkilled");
    }

    /* Observability: the group must have recorded the kill. */
    if (read_text(CG_GROUP "/memory.events", buf, sizeof(buf)) != 0) {
        cleanup_cgroup();
        return fail("oom-memory.events-read");
    }
    long oom_kill = parse_key_value(buf, "oom_kill");
    if (oom_kill == LONG_MIN || oom_kill < 1) {
        printf("OOM_STRESS: memory.events=[%s]\n", buf);
        cleanup_cgroup();
        return fail("oom-events-no-kill");
    }
    long failcnt = parse_key_value(buf, "max ");
    if (failcnt == LONG_MIN || failcnt < 1) {
        cleanup_cgroup();
        return fail("oom-events-no-failcnt");
    }

    /* Global OOM surface still answers after the kill. */
    if (read_text("/proc/a20/oom", buf, sizeof(buf)) != 0) {
        cleanup_cgroup();
        return fail("oom-proc-a20-oom");
    }

    /* System liveness: fresh allocations and file I/O still work. */
    enum { PROBE = 4 * 1024 * 1024 };
    volatile char *probe = mmap(NULL, PROBE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (probe == MAP_FAILED) {
        cleanup_cgroup();
        return fail("oom-survivor-mmap");
    }
    for (size_t off = 0; off < PROBE; off += 4096)
        probe[off] = (char)off;
    munmap((void *)probe, PROBE);

    const char *tmp = "/tmp/oom_stress_probe.txt";
    unlink(tmp);
    int fd = open(tmp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        cleanup_cgroup();
        return fail("oom-survivor-create");
    }
    if (write(fd, "alive", 5) != 5) {
        close(fd);
        unlink(tmp);
        cleanup_cgroup();
        return fail("oom-survivor-write");
    }
    close(fd);

    cleanup_cgroup();
    unlink(tmp);
    printf("OOM_STRESS: victim sigkill PASS\n");
    printf("OOM_STRESS: events observability PASS\n");
    printf("OOM_STRESS: survivor liveness PASS\n");
    return 0;
}

int main(void)
{
    printf("OOM_STRESS: start\n");
    mkdir("/tmp", 0755);
    if (cgroup_oom_kill() != 0)
        return 1;
    printf("OOM_STRESS: PASS\n");
    return 0;
}
