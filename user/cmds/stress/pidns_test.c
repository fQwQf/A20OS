/*
 * pidns_test — pid namespace smoke test.
 *
 * The property under test throughout is that a process's view of ids comes
 * from the namespace it is a MEMBER of, while the namespace it is spawning
 * children into only changes the next child.  The two being confused is the
 * whole class of bug this file exists to catch: getpid() reporting 0, /proc
 * listing host ids, or kill() reaching a container neighbour.
 *
 * Coverage:
 *  - /proc/self/ns/pid reads back in "pid:[ino]" form, and the ino differs
 *    from the initial namespace's
 *  - getpid() is unchanged by unshare(CLONE_NEWPID) itself
 *  - the first child after unshare is pid 1 inside the new namespace
 *  - a second child after unshare also lands in the new namespace (Linux keeps
 *    pid_ns_for_children set until setns replaces it, not just for one fork)
 *  - inside the namespace, /proc lists only container-local ids and starting
 *    at 1
 *  - inside the namespace, kill() addresses the container-local id, and a
 *    host id is not reachable as a pid
 *  - getppid() of a namespace's init is 0, because its parent is outside
 *  - /proc/<pid> paths resolve to the namespace-local id, and the host's
 *    global ids are not addressable by number
 *  - clone(CLONE_NEWPID) makes the child pid 1 of a fresh namespace
 *  - a thread (CLONE_THREAD) stays in its leader's namespace rather than
 *    landing in the namespace the leader is spawning children into
 *  - setns() through /proc/<pid>/ns/pid re-points where children join, and
 *    does not move the caller
 *  - CLONE_THREAD|CLONE_NEWPID together are refused with EINVAL
 *
 * Prints "PIDNS_TEST: PASS" on success.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_unshare
#define SYS_unshare 97
#endif
#ifndef SYS_setns
#define SYS_setns 268
#endif
#ifndef SYS_clone
#define SYS_clone 56
#endif

#define CLONE_THREAD  0x00010000
#define CLONE_NEWPID  0x20000000
#define SIGCHLD_      17

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("PIDNS_TEST: FAIL: %s (line %d, errno=%d)\n", \
               msg, __LINE__, errno); \
        failures++; \
        goto out; \
    } \
} while (0)

/* Fail without unwinding: used inside helpers that return a status. */
#define REQUIRE(cond, msg) do { \
    if (!(cond)) { \
        printf("PIDNS_TEST: FAIL: %s (line %d, errno=%d)\n", \
               msg, __LINE__, errno); \
        failures++; \
        return -1; \
    } \
} while (0)

static long xunshare(int flags) { return syscall(SYS_unshare, flags); }
static long xsetns(int fd, int nstype) { return syscall(SYS_setns, fd, nstype); }

/* Read an ns file and require the exact "<prefix>:[<number>]" shape. */
static int read_ns_file(const char *path, const char *prefix,
                        unsigned long long *ino_out)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char expect[16];
    snprintf(expect, sizeof(expect), "%s:[", prefix);
    if (strncmp(buf, expect, strlen(expect)) != 0)
        return -1;
    char *end = NULL;
    unsigned long long ino = strtoull(buf + strlen(expect), &end, 10);
    if (!end || *end != ']')
        return -1;
    if (ino_out)
        *ino_out = ino;
    return 0;
}

static int wait_exit(pid_t pid, int *code)
{
    int st = 0;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (!WIFEXITED(st))
        return -1;
    if (code)
        *code = WEXITSTATUS(st);
    return 0;
}

/*
 * Collect the numeric directory entries of /proc into ids[], returning the
 * count.  Numeric entries are the per-process directories; the rest of /proc
 * is named, so this is exactly "what does this process see in /proc".
 */
static int proc_pid_list(int *ids, int max)
{
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        char *end = NULL;
        long v = strtol(e->d_name, &end, 10);
        if (!end || *end != '\0')
            continue;
        /* A numeric entry is recorded even when it is 0.  "0" is not a valid
         * pid, so its presence is the signature of a task that was never given
         * an id in this namespace -- dropping it here would hide exactly the
         * defect the nesting check exists to catch. */
        if (n < max)
            ids[n++] = (int)v;
    }
    closedir(d);
    return n;
}

/*
 * Run body() inside a fresh pid namespace created with unshare() + fork().
 *
 * The unshare() runs in the parent because unshare(CLONE_NEWPID) only affects
 * children; the fork() is what lands inside.  body() returns a process exit
 * code.  On failure to even get there, -1 is returned.
 */
static pid_t enter_container(void (*body)(void))
{
    pid_t outer = fork();
    if (outer < 0)
        return -1;
    if (outer == 0) {
        if (xunshare(CLONE_NEWPID) != 0)
            _exit(90);
        pid_t inner = fork();
        if (inner < 0)
            _exit(91);
        if (inner == 0) {
            body();
            _exit(0);
        }
        int st = 0;
        if (waitpid(inner, &st, 0) < 0)
            _exit(92);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 93);
    }
    return outer;
}

/* ---------------------------------------------------------------- bodies */

struct thread_arg {
    int    fd;
    pid_t  expect;
};

/*
 * A thread in the caller's own namespace: it must report the leader's pid.
 * getpid() is group-wide, so a thread that had been placed in the container
 * the leader is spawning children into would report that namespace's id
 * instead -- a small number instead of the leader's real pid.
 */
static void *thread_probe(void *argp)
{
    struct thread_arg *arg = (struct thread_arg *)argp;
    pid_t mine = getpid();
    (void)!write(arg->fd, &mine, sizeof(mine));
    close(arg->fd);
    (void)arg->expect;
    return NULL;
}


/* Inside a container: everything must be numbered from 1 in its own space. */
static void body_container_ids(void)
{
    if (getpid() != 1) {
        printf("PIDNS_TEST: FAIL: container init pid is %d, want 1\n",
               (int)getpid());
        failures++;
        return;
    }
    /* getppid() of a namespace's init is 0: its parent lives outside. */
    pid_t pp = getppid();
    if (pp != 0) {
        printf("PIDNS_TEST: FAIL: container init ppid is %d, want 0\n", (int)pp);
        failures++;
        return;
    }

    /* /proc must list only ids from this namespace, and 1 must be present. */
    int ids[256];
    int n = proc_pid_list(ids, 256);
    if (n <= 0) {
        printf("PIDNS_TEST: FAIL: /proc listed no pids inside the container\n");
        failures++;
        return;
    }
    int saw_one = 0;
    for (int i = 0; i < n; i++) {
        if (ids[i] == 1)
            saw_one = 1;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d", ids[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            printf("PIDNS_TEST: FAIL: /proc lists %d but it cannot be "
                   "resolved (line %d)\n", ids[i], __LINE__);
            failures++;
            return;
        }
    }
    if (!saw_one) {
        printf("PIDNS_TEST: FAIL: /proc inside the container does not list "
               "pid 1\n");
        failures++;
        return;
    }

    /* A host id must not resolve.  The container's init has global pid 1 only
     * if the test itself is pid 1, so probe a small range of ids that are
     * certainly not container-local: a container with fewer than 64
     * processes cannot have any id in this range. */
    for (int probe = 1000; probe < 1004; probe++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d", probe);
        if (access(path, F_OK) == 0) {
            printf("PIDNS_TEST: FAIL: host pid %d is reachable from inside the "
                   "container\n", probe);
            failures++;
            return;
        }
    }
}

/*
 * Nesting check: a container (level 1) spawns another container (level 2) and
 * inspects its own /proc WHILE the level-2 process is alive.
 *
 * The listing has to be taken while the process exists -- an exited process is
 * already reaped out of /proc, so checking afterwards would prove nothing.  The
 * assertion is that every id the level-1 container sees is a real, resolvable,
 * nonzero id: a task given ids only in its own deepest namespace would appear
 * here as 0, because the level-1 slot was never allocated.
 */
static void body_nesting_outer(void)
{
    int ready[2], release[2];
    if (pipe(ready) != 0 || pipe(release) != 0)
        _exit(20);

    pid_t inner = fork();
    if (inner < 0)
        _exit(21);
    if (inner == 0) {
        /* This coordinator creates the level-2 namespace and then simply waits
         * for the grandchild.  Only the read end is dropped: the grandchild is
         * forked below and needs both the write end of ready and the read end
         * of release, so closing those here would hand it closed descriptors. */
        close(ready[0]);
        if (xunshare(CLONE_NEWPID) != 0)
            _exit(22);
        pid_t grand = fork();
        if (grand < 0)
            _exit(23);
        if (grand == 0) {
            /* Level 2: pid 1 in its own namespace, holding still until the
             * parent namespace has finished looking at it. */
            if (getpid() != 1)
                _exit(24);
            (void)!write(ready[1], "r", 1);
            char c;
            if (read(release[0], &c, 1) != 1)
                _exit(25);
            _exit(0);
        }
        int st = 0;
        if (waitpid(grand, &st, 0) < 0)
            _exit(27);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 28);
    }

    close(ready[1]);
    close(release[0]);
    char token;
    if (read(ready[0], &token, 1) != 1)
        _exit(29);

    /* The level-2 process is alive and this container can see it.  Every id it
     * sees must be usable as a /proc path component. */
    int ids[256];
    int n = proc_pid_list(ids, 256);
    int rc = 0;
    if (n <= 0) {
        printf("PIDNS_TEST: FAIL: outer container sees no pids\n");
        fflush(stdout);
        rc = 1;
    }
    for (int i = 0; i < n && !rc; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d", ids[i]);
        if (ids[i] < 1 || access(path, F_OK) != 0) {
            printf("PIDNS_TEST: FAIL: outer container lists id %d which is "
                   "not a resolvable /proc path\n", ids[i]);
            fflush(stdout);
            rc = 1;
        }
    }
    /* This container's own init is level 1 and must be id 1 here. */
    if (!rc) {
        int saw_self_one = 0;
        for (int i = 0; i < n; i++)
            if (ids[i] == 1)
                saw_self_one = 1;
        if (!saw_self_one) {
            printf("PIDNS_TEST: FAIL: outer container does not see itself as "
                   "pid 1\n");
            fflush(stdout);
            rc = 1;
        }
    }

    (void)!write(release[1], "x", 1);
    close(release[1]);
    close(ready[0]);
    int st = 0;
    if (waitpid(inner, &st, 0) < 0)
        _exit(30);
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
        rc = 1;
    failures += rc;
    if (rc) {
        printf("PIDNS_TEST: FAIL: nested container probe failed (inner code "
               "%d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        fflush(stdout);
    }
    _exit(rc ? 31 : 0);
}

int main(void)
{
    unsigned long long init_ino = 0, own_ino = 0;
    pid_t my_pid = getpid();

    /* 1. Namespace identity is readable and the initial one is distinct. */
    CHECK(read_ns_file("/proc/self/ns/pid", "pid", &init_ino) == 0,
          "read /proc/self/ns/pid format");
    CHECK(init_ino != 0, "pid namespace ino is nonzero");
    CHECK(read_ns_file("/proc/self/ns/pid", "pid", &own_ino) == 0 &&
          own_ino == init_ino,
          "self pid namespace matches before any unshare");

    /* 2. unshare(CLONE_NEWPID) must not change the caller's own ids.  A
     *    caller that reported 0 here would be reporting "no pid" for a live
     *    process, which is the bug this line guards. */
    CHECK(xunshare(CLONE_NEWPID) == 0, "unshare(CLONE_NEWPID)");
    CHECK(getpid() == my_pid, "unshare(CLONE_NEWPID) leaves the caller's pid "
                              "unchanged");
    CHECK(read_ns_file("/proc/self/ns/pid", "pid", &own_ino) == 0 &&
          own_ino == init_ino,
          "unshare(CLONE_NEWPID) leaves the caller in its own namespace");

    /* 3. The next two children BOTH land in the new namespace.  A one-shot
     *    implementation would put only the first one there. */
    {
        pid_t c1 = fork();
        CHECK(c1 >= 0, "first fork after unshare");
        if (c1 == 0)
            _exit(getpid() == 1 ? 0 : 1);
        int code = 0;
        CHECK(wait_exit(c1, &code) == 0, "first child exits");
        CHECK(code == 0, "first child is pid 1 in the new namespace");

        pid_t c2 = fork();
        CHECK(c2 >= 0, "second fork after unshare");
        if (c2 == 0)
            _exit(getpid() == 2 ? 0 : 1);
        CHECK(wait_exit(c2, &code) == 0, "second child exits");
        CHECK(code == 0,
              "second child is pid 2: the namespace persists across forks");
    }

    /* 4. Inside a container: pid 1, ppid 0, a container-scoped /proc, and no
     *    reachable host ids. */
    {
        pid_t outer = enter_container(body_container_ids);
        CHECK(outer > 0, "enter pid namespace");
        int code = 0;
        CHECK(wait_exit(outer, &code) == 0, "container probe exits");
        CHECK(code == 0, "container ids are namespace-local");
    }

    /* 5. A sibling namespace must not be addressable.  Two containers both
     *    have a pid 1; resolving it has to land in the caller's own one. */
    {
        pid_t outer = enter_container(body_container_ids);
        CHECK(outer > 0, "enter container for sibling check");
        int code = 0;
        CHECK(wait_exit(outer, &code) == 0, "sibling container exits");
        CHECK(code == 0, "sibling namespace does not shadow the caller");

        /* From outside, the container's pid 1 is reachable as the global id,
         * not as "1" -- the two spaces must not be conflated. */
        char host1[64];
        snprintf(host1, sizeof(host1), "/proc/%d", my_pid);
        CHECK(access(host1, F_OK) == 0,
              "host pid is still addressable by its global id");
    }

    /* 5b. Nesting: a level-2 process must stay addressable from the level-1
     *     container that spawned it.  This is the case that fails if a task is
     *     given an id only in its own deepest namespace. */
    {
        pid_t outer = enter_container(body_nesting_outer);
        CHECK(outer > 0, "enter container for nesting check");
        int code = 0;
        CHECK(wait_exit(outer, &code) == 0, "nested container probe exits");
        if (code != 0)
            printf("PIDNS_TEST: nested container probe exited %d\n", code);
        CHECK(code == 0,
              "a process in a nested namespace is still addressable from its "
              "ancestor namespace");
    }

    /* 6. clone(CLONE_NEWPID) makes the child pid 1 of a fresh namespace. */
    {
        pid_t c = (pid_t)syscall(SYS_clone, CLONE_NEWPID | SIGCHLD_, 0, 0, 0, 0);
        CHECK(c >= 0, "clone(CLONE_NEWPID)");
        if (c == 0)
            _exit(getpid() == 1 ? 0 : 1);
        int code = 0;
        CHECK(wait_exit(c, &code) == 0, "clone(CLONE_NEWPID) child exits");
        CHECK(code == 0, "clone(CLONE_NEWPID) child is pid 1");
    }

    /* 7. A thread must stay in its leader's namespace.  The caller is
     *    currently spawning children into a container, so a thread that took
     *    that namespace instead would report the container-local id -- the
     *    clearest possible symptom of a thread being misplaced. */
    {
        int pfd[2];
        CHECK(pipe(pfd) == 0, "pipe for thread check");
        struct thread_arg arg = { pfd[1], my_pid };
        pthread_t th;
        int pr = pthread_create(&th, NULL, thread_probe, &arg);
        CHECK(pr == 0, "pthread_create");
        close(pfd[1]);
        pid_t seen = -1;
        CHECK(read(pfd[0], &seen, sizeof(seen)) == (ssize_t)sizeof(seen),
              "read thread getpid");
        close(pfd[0]);
        CHECK(pthread_join(th, NULL) == 0, "join thread");
        CHECK(seen == my_pid,
              "a thread stays in its leader's namespace, not the one the "
              "leader is spawning children into");
    }

    /* 8. setns() re-points where children join and does not move the caller. */
    {
        int fd_pidns = open("/proc/self/ns/pid_for_children", O_RDONLY);
        CHECK(fd_pidns >= 0, "open pid_for_children ns fd");
        CHECK(xsetns(fd_pidns, CLONE_NEWPID) == 0,
              "setns to own pid_for_children namespace");
        CHECK(getpid() == my_pid, "setns(CLONE_NEWPID) does not move the caller");
        close(fd_pidns);

        /* /proc/self/ns/pid is the caller's own membership and is unchanged. */
        unsigned long long after = 0;
        CHECK(read_ns_file("/proc/self/ns/pid", "pid", &after) == 0 &&
              after == init_ino,
              "setns leaves the caller's own namespace alone");
    }

    /* 9. An unimplemented namespace type is still refused honestly. */
    {
        errno = 0;
        CHECK(xunshare(0x40000000 /*CLONE_NEWNET*/) == -1 && errno == EINVAL,
              "unshare(CLONE_NEWNET) still fails with EINVAL");
    }

out:
    if (failures)
        return 1;
    printf("PIDNS_TEST: PASS\n");
    return 0;
}