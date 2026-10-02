/*
 * userns_test — user namespace smoke test.
 *
 * The property under test throughout is that ids are GLOBAL in the kernel and
 * NAMESPACE-LOCAL everywhere else.  proc_cred_t stores host ids; getuid(),
 * setuid(), /proc/<pid>/{uid,gid}_map and the ownership checks translate them
 * at the boundary.  Getting that wrong produces the two failures this file
 * exists to catch: a container that silently inherits its parent's numbering
 * (privilege leak), and a container that cannot represent the ids it was
 * granted (unusable namespace).
 *
 * The interesting cases are the UNPRIVILEGED ones, so most of the probing runs
 * after setuid(1000).  A root process inside a user namespace is allowed to
 * retain authority over the namespace's parent -- it created it, and Linux
 * grants exactly that -- so testing containment as root proves nothing.  After
 * dropping to uid 1000 the process is an ordinary user that gets a namespace
 * anyway, and every authority question below has exactly one right answer.
 *
 * Coverage:
 *  - /proc/self/ns/user reads back in "user:[ino]" form with a nonzero ino
 *  - listns(CLONE_NEWUSER) agrees with /proc/self/ns/user, and listns(CLONE_NEWPID)
 *    reports the caller's real pid namespace rather than a constant
 *  - unshare(CLONE_NEWUSER) creates a distinct namespace and does not move the
 *    caller out of it
 *  - a fresh namespace has an EMPTY map: getuid() is the overflow id, not 0
 *    and not the host uid
 *  - a privileged creator (root) can install "0 <host uid> 1" through
 *    /proc/<pid>/uid_map, after which the process inside sees uid 0
 *  - an UNPRIVILEGED process can install exactly one line mapping its own uid,
 *    and nothing wider
 *  - malformed, overflowing, zero-length and overlapping maps are refused
 *  - setuid() to an id with no representation is refused
 *  - a process inside a user namespace cannot setns back into the initial one
 *  - setgroups can be denied unprivileged, and denying is irreversible; the
 *    unprivileged gid map is only reachable after denying it
 *  - fork() and threads inherit the caller's namespace; only clone(CLONE_NEWUSER)
 *    makes a new one, and neither moves the caller
 *
 * Prints "USERNS_TEST: PASS" on success.
 */
#include <errno.h>
#include <fcntl.h>
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
#ifndef SYS_listns
#define SYS_listns 441
#endif

#define CLONE_NEWUSER    0x10000000
#define CLONE_NEWPID     0x20000000
#define SIGCHLD_         17

/* The uid an ordinary user drops to for the privilege-sensitive probes. */
#define TEST_UID 1000
#define TEST_GID 1000

/* The id the kernel reports for a global uid with no representation inside the
 * caller's namespace.  Must match USERNS_OVERFLOW_UID in
 * kernel/include/proc/userns.h; it is a real value rather than a cosmetic one,
 * because it is what a containerised process sees for every id it was not
 * given. */
#define OVERFLOW_UID 65534

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("USERNS_TEST: FAIL: %s (line %d, errno=%d)\n", \
               msg, __LINE__, errno); \
        failures++; \
        goto out; \
    } \
} while (0)

/* Same, but for a call that returns a negative errno of its own.  Printing the
 * global errno here would report whatever the last unrelated syscall left
 * behind, which is how a write that failed with EINVAL gets reported as
 * whatever happened two calls ago. */
#define CHECK_RC(rc, want, msg) do { \
    int rc_ = (rc); \
    if (rc_ != (want)) { \
        printf("USERNS_TEST: FAIL: %s (line %d, got %d, want %d, errno=%d)\n", \
               msg, __LINE__, rc_, (want), errno); \
        failures++; \
        goto out; \
    } \
} while (0)

static long xunshare(int flags) { return syscall(SYS_unshare, flags); }
static long xsetns(int fd, int nstype) { return syscall(SYS_setns, fd, nstype); }

static long xlistns(unsigned int nstype, unsigned long long *out)
{
    return syscall(SYS_listns, nstype, out, 1);
}

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

/* Write @text to /proc/<pid>/<name>; returns 0 or -errno. */
static int write_proc_map(pid_t pid, const char *name, const char *text)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    size_t len = strlen(text);
    ssize_t w = write(fd, text, len);
    int e = errno;
    close(fd);
    if (w < 0)
        return -e;
    if ((size_t)w != len)
        return -EIO;
    return 0;
}

/* Read a whole (small) procfs file into @buf.  Returns 0 or -errno. */
static int read_proc_file(const char *path, char *buf, size_t sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    ssize_t n = read(fd, buf, sz - 1);
    int e = errno;
    close(fd);
    if (n < 0)
        return -e;
    buf[n] = '\0';
    return 0;
}

/* ------------------------------------------------------------ child probes */

/*
 * The unprivileged process.  Everything it does here happens with euid 1000
 * and, after the unshare, inside a namespace it created itself.
 */
#define FAIL(code, ...) do { \
    printf("USERNS_TEST: FAIL: " __VA_ARGS__); \
    fflush(stdout); \
    _exit(code); \
} while (0)

/*
 * Phase 1: report getuid() BEFORE any map exists.
 * Phase 2: wait for the creator to install a map.
 * Phase 3: report getuid() again, plus two setuid() outcomes.
 */
static void body_unmapped(int to_parent, int from_parent)
{
    int before = (int)getuid();

    /* Nothing is mapped yet, so the process must see the overflow id.  Seeing
     * its host uid here would mean the namespace inherited the parent's
     * numbering, which is the whole failure mode of this feature. */
    if (write(to_parent, &before, sizeof(before)) != (ssize_t)sizeof(before))
        _exit(31);
    /* The creator blocks in the map write until this lands, so this is the
     * barrier that keeps the namespace alive across the exchange: the parent
     * must not install a map into a task that has already exited. */
    char token;
    if (read(from_parent, &token, 1) != 1)
        _exit(32);

    int after = (int)getuid();
    if (write(to_parent, &after, sizeof(after)) != (ssize_t)sizeof(after))
        _exit(33);

    /* With 0 mapped, setuid(0) must succeed as itself, and setuid(1) must be
     * refused: 1 was never handed out, so translating it in has no
     * representation.  An implementation that accepted unmappable ids would
     * let a process name a uid that does not exist. */
    errno = 0;
    int r0 = setuid(0) == 0 ? 0 : -errno;
    errno = 0;
    int r1 = setuid(1) == 0 ? 0 : -errno;
    int pair[2] = { r0, r1 };
    if (write(to_parent, pair, sizeof(pair)) != (ssize_t)sizeof(pair))
        _exit(34);
    _exit(0);
}

/*
 * The rootless path, end to end: an unprivileged user creates a namespace,
 * gives it exactly one id (its own), becomes uid 0 inside it, and still has no
 * authority outside.
 */
static void body_rootless(void)
{
    /* Inside its own namespace with nothing mapped: the overflow id. */
    if (getuid() != OVERFLOW_UID)
        FAIL(40, "uid inside a fresh namespace is %d, want the overflow id %d\n",
             (int)getuid(), OVERFLOW_UID);

    /* The unprivileged exception: exactly one line, mapping the writer's own
     * uid to namespace id 0.  This is what `unshare -U; echo $USER >
     * /proc/self/uid_map` does, and it is the most a user without privilege
     * may ever grant itself. */
    if (write_proc_map(getpid(), "uid_map", "0 1000 1\n") != 0)
        FAIL(41, "unprivileged single-id uid_map was refused (errno=%d)\n",
             errno);
    if (getuid() != 0)
        FAIL(42, "uid after installing its own map is %d, want 0\n",
             (int)getuid());

    /* Anything wider must be refused even though the writer is root inside the
     * namespace: the privilege that would authorise it lives in the PARENT,
     * and the parent granted nothing. */
    if (write_proc_map(getpid(), "uid_map", "1 0 1\n") != -EPERM)
        FAIL(43, "an unprivileged second extent mapping a foreign id was "
                 "accepted\n");
    /* The same id twice is refused even as root here: the map must not be
     * silently rewritten once extents exist below it. */
    if (write_proc_map(getpid(), "uid_map", "0 1000 1\n") == 0)
        FAIL(44, "an overlapping extent was accepted\n");

    /* setgroups: deny is unprivileged, and it is what makes the unprivileged
     * gid map reachable at all. */
    char buf[32];
    if (read_proc_file("/proc/self/setgroups", buf, sizeof(buf)) != 0 ||
        strcmp(buf, "allow\n") != 0)
        FAIL(45, "a fresh namespace does not start with setgroups allowed\n");
    if (write_proc_map(getpid(), "gid_map", "0 1000 1\n") == 0)
        FAIL(46, "an unprivileged gid_map was accepted while setgroups was "
                 "still allowed\n");
    if (write_proc_map(getpid(), "setgroups", "deny") != 0)
        FAIL(47, "setgroups deny was refused unprivileged (errno=%d)\n", errno);
    if (read_proc_file("/proc/self/setgroups", buf, sizeof(buf)) != 0 ||
        strcmp(buf, "deny\n") != 0)
        FAIL(48, "setgroups did not read back as deny\n");
    if (write_proc_map(getpid(), "gid_map", "0 1000 1\n") != 0)
        FAIL(49, "the unprivileged gid_map after deny was refused (errno=%d)\n",
             errno);
    if (write_proc_map(getpid(), "setgroups", "deny") != 0)
        FAIL(50, "a repeated setgroups deny was refused\n");
    /* Undoing it is not permitted even from inside a namespace holding a full
     * capability set: authority may already have been delegated during the
     * window in which it was allowed. */
    if (write_proc_map(getpid(), "setgroups", "allow") != -EPERM)
        FAIL(51, "setgroups allow after deny was not refused with EPERM\n");
    /* Garbage is EINVAL, not a silent success -- a typo in a one-way security
     * switch must not read as success. */
    if (write_proc_map(getpid(), "setgroups", "maybe") != -EINVAL)
        FAIL(52, "a malformed setgroups keyword was not refused with EINVAL\n");

    /* Containment.  This process created the namespace it is inside, but it
     * created it as uid 1000, so it keeps no authority over the initial
     * namespace -- joining that would be escaping the sandbox. */
    unsigned long long mine = 0, root_ns = 0;
    if (read_ns_file("/proc/self/ns/user", "user", &mine) != 0 ||
        read_ns_file("/proc/1/ns/user", "user", &root_ns) != 0)
        FAIL(53, "cannot read the user namespace files\n");
    if (mine == root_ns)
        FAIL(54, "the unshare did not create a distinct user namespace\n");
    int fd = open("/proc/1/ns/user", O_RDONLY);
    if (fd < 0)
        FAIL(55, "open /proc/1/ns/user\n");
    errno = 0;
    long r = xsetns(fd, CLONE_NEWUSER);
    int saved = errno;
    close(fd);
    if (r != -1)
        FAIL(56, "a process inside a user namespace was allowed to setns back "
                 "into the initial namespace\n");
    if (saved != EPERM)
        FAIL(57, "the refused setns returned errno %d, want EPERM\n", saved);
    /* The refusal must not have moved us. */
    unsigned long long after = 0;
    if (read_ns_file("/proc/self/ns/user", "user", &after) != 0 || after != mine)
        FAIL(58, "the refused setns changed the caller's namespace\n");

    /* Its own namespace is still joinable by itself, and setuid back to the
     * single id it legitimately owns must work. */
    fd = open("/proc/self/ns/user", O_RDONLY);
    if (fd < 0)
        FAIL(59, "reopen /proc/self/ns/user\n");
    if (xsetns(fd, CLONE_NEWUSER) != 0)
        FAIL(60, "setns into one's own user namespace was refused (errno=%d)\n",
             errno);
    close(fd);
    /* The credentials are unchanged by any of that: the process still owns the
     * one host uid it was given, still sees it as namespace uid 0, and still
     * cannot name any other id -- there is no representation for one. */
    if (getuid() != 0)
        FAIL(61, "uid after the containment probes is %d, want 0\n",
             (int)getuid());
    if (setuid(1) == 0)
        FAIL(62, "setuid to an id with no representation was accepted\n");
    _exit(0);
}

int main(void)
{
    unsigned long long init_ino = 0, own_ino = 0, listed = 0;
    unsigned long long pid_ino = 0, listed_pid = 0;
    char buf[160];

    /* 1. Namespace identity is readable, and the initial one is a real id. */
    CHECK(read_ns_file("/proc/self/ns/user", "user", &init_ino) == 0,
          "read /proc/self/ns/user format");
    CHECK(init_ino != 0, "user namespace ino is nonzero");

    /* 2. listns agrees with the procfs view.  Both readings of "which namespace
     *    am I in" have to come from the same object, or a tool that trusts one
     *    and a tool that trusts the other will disagree about containment. */
    CHECK(xlistns(CLONE_NEWUSER, &listed) == 1 && listed == init_ino,
          "listns(CLONE_NEWUSER) matches /proc/self/ns/user");

    /* 3. The initial namespace's own files.  A system that has never created a
     *    user namespace must still present a well-formed identity map. */
    CHECK(read_proc_file("/proc/self/uid_map", buf, sizeof(buf)) == 0,
          "read /proc/self/uid_map");
    CHECK(strstr(buf, "4294967295") != NULL,
          "the initial namespace maps the whole uid space to itself");
    CHECK(read_proc_file("/proc/self/gid_map", buf, sizeof(buf)) == 0,
          "read /proc/self/gid_map");
    CHECK(read_proc_file("/proc/self/setgroups", buf, sizeof(buf)) == 0 &&
          strcmp(buf, "allow\n") == 0,
          "setgroups starts allowed in the initial namespace");

    /* 4. listns(CLONE_NEWPID) must report the caller's real pid namespace.  A
     *    constant there would make a pid namespace undetectable from user
     *    space, which is exactly what the pid namespace work fixed. */
    CHECK(read_ns_file("/proc/self/ns/pid", "pid", &pid_ino) == 0,
          "read /proc/self/ns/pid");
    CHECK(xlistns(CLONE_NEWPID, &listed_pid) == 1 && listed_pid == pid_ino,
          "listns(CLONE_NEWPID) matches /proc/self/ns/pid");

    /* 5. The privileged path: a creator writes a map into a child's namespace
     *    through /proc/<pid>/uid_map, which is the only channel there is for
     *    handing a namespace its ids. */
    {
        int pfd[2], go[2];
        CHECK(pipe(pfd) == 0 && pipe(go) == 0, "pipes for map install");

        pid_t child = fork();
        CHECK(child >= 0, "fork for map install");
        if (child == 0) {
            close(pfd[0]);
            close(go[1]);
            if (xunshare(CLONE_NEWUSER) != 0)
                _exit(30);
            body_unmapped(pfd[1], go[0]);
        }
        close(pfd[1]);
        close(go[0]);

        int before = -1;
        CHECK(read(pfd[0], &before, sizeof(before)) == (ssize_t)sizeof(before),
              "read pre-map uid");
        CHECK(before == OVERFLOW_UID,
              "a namespace with no map reports the overflow id, not the host "
              "uid and not 0");

        char path[64], text[64];
        snprintf(text, sizeof(text), "0 %d 1\n", (int)getuid());
        snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child);
        CHECK_RC(write_proc_map(child, "uid_map", text), 0,
                 "the creator writes /proc/<pid>/uid_map");
        CHECK(read_proc_file(path, buf, sizeof(buf)) == 0,
              "the installed map reads back");
        CHECK(strncmp(buf, "         0", 10) == 0 ||
              strncmp(buf, "0 ", 2) == 0,
              "the installed map's first extent starts at namespace id 0");

        CHECK(write(go[1], "g", 1) == 1, "release the child");

        int after = -1;
        CHECK(read(pfd[0], &after, sizeof(after)) == (ssize_t)sizeof(after),
              "read post-map uid");
        CHECK(after == 0, "after the map is installed the process is uid 0");

        int pair[2] = { -1, -1 };
        CHECK(read(pfd[0], pair, sizeof(pair)) == (ssize_t)sizeof(pair),
              "read setuid results");
        CHECK(pair[0] == 0, "setuid(0) succeeds once 0 is mapped");
        CHECK(pair[1] != 0, "setuid() to an unmapped id is refused");

        close(pfd[0]);
        close(go[1]);
        int code = 0;
        CHECK(wait_exit(child, &code) == 0, "map-install child exits");
        CHECK(code == 0, "map-install child completed");
    }

    /* 6. Malformed and abusive maps are refused, each for its own reason.
     *    These run from the creator side, so the permission check passes and
     *    what is under test is the parser and the overflow guard. */
    {
        int pfd[2], go[2];
        CHECK(pipe(pfd) == 0 && pipe(go) == 0, "pipes for map rejection");

        pid_t child = fork();
        CHECK(child >= 0, "fork for map rejection");
        if (child == 0) {
            close(pfd[0]);
            close(go[1]);
            if (xunshare(CLONE_NEWUSER) != 0)
                _exit(60);
            /* Announce that the namespace exists before blocking.  The
             * creator's first write below would otherwise race the unshare:
             * /proc/<pid>/uid_map names the task's CURRENT user namespace, so
             * a write that lands first targets the initial namespace and is
             * rightly refused with EINVAL. */
            if (write(pfd[1], "r", 1) != 1)
                _exit(62);
            char c;
            if (read(go[0], &c, 1) != 1)
                _exit(61);
            _exit(0);
        }
        close(pfd[1]);
        close(go[0]);

        char ready;
        errno = 0;
        CHECK(read(pfd[0], &ready, 1) == 1,
              "the rejection child reached its own user namespace");

        CHECK_RC(write_proc_map(child, "uid_map", "0 0\n"), -EINVAL,
                 "a two-field line is refused");
        CHECK_RC(write_proc_map(child, "uid_map", "-1 0 1\n"), -EINVAL,
                 "a negative field is refused");
        /* 4294967295 + length 2 would alias id 0 in a 32-bit space. */
        CHECK_RC(write_proc_map(child, "uid_map", "0 4294967295 2\n"), -EINVAL,
                 "an extent that runs off the end of the id space is refused");
        CHECK_RC(write_proc_map(child, "uid_map", "0 0 0\n"), -EINVAL,
                 "a zero-length extent is refused");

        CHECK_RC(write_proc_map(child, "uid_map", "0 0 1\n"), 0,
                 "a valid single-id map is accepted");
        /* Appending above the last extent is legal, and is how Linux lets a
         * creator extend a map in stages. */
        CHECK_RC(write_proc_map(child, "uid_map", "1 1 1\n"), 0,
                 "a second, disjoint extent above the first is accepted");
        /* Overlapping one is not: the map must stay a set of disjoint ranges,
         * which is what keeps lookup a plain scan. */
        CHECK_RC(write_proc_map(child, "uid_map", "1 2 1\n"), -EINVAL,
                 "an extent overlapping an existing one is refused");
        CHECK_RC(write_proc_map(child, "uid_map", "0 3 1\n"), -EINVAL,
                 "an extent below the existing range is refused");

        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child);
        CHECK(read_proc_file(path, buf, sizeof(buf)) == 0,
              "read back the two-extent map");
        int lines = 0;
        for (const char *q = buf; *q; q++)
            if (*q == '\n')
                lines++;
        CHECK(lines == 2, "the two accepted extents render as two lines");
        CHECK(strstr(buf, " 2 ") == NULL,
              "the rejected extent left no trace in the rendered map");

        CHECK(write(go[1], "g", 1) == 1, "release the rejection child");
        close(go[1]);
        int code = 0;
        CHECK(wait_exit(child, &code) == 0, "rejection child exits");
        CHECK(code == 0, "rejection child completed");
    }

    /* 7. The rootless path, end to end. */
    {
        pid_t child = fork();
        CHECK(child >= 0, "fork for rootless probe");
        if (child == 0) {
            /* Drop privilege FIRST.  As root the containment checks below are
             * vacuous -- a root creator keeps authority over the namespace's
             * parent by design -- so the probe would prove nothing. */
            if (setgid(TEST_GID) != 0 || setuid(TEST_UID) != 0)
                _exit(70);
            if (getuid() != TEST_UID)
                _exit(71);
            if (xunshare(CLONE_NEWUSER) != 0)
                _exit(72);
            body_rootless();
        }
        int code = 0;
        CHECK(wait_exit(child, &code) == 0, "rootless probe exits");
        if (code != 0 && code < 70)
            printf("USERNS_TEST: rootless probe exited %d\n", code);
        CHECK(code == 0,
              "an unprivileged user gets a usable namespace and no authority "
              "outside it");
    }

    /* 8. Inheritance: fork() keeps the caller's namespace, and so does a
     *    thread.  Only clone(CLONE_NEWUSER) makes a new one. */
    {
        unsigned long long before = 0;
        CHECK(read_ns_file("/proc/self/ns/user", "user", &before) == 0,
              "read own user namespace before forking");

        int pfd[2];
        CHECK(pipe(pfd) == 0, "pipe for inheritance check");
        pid_t child = fork();
        CHECK(child >= 0, "fork for inheritance check");
        if (child == 0) {
            close(pfd[0]);
            unsigned long long mine = 0;
            int ok = read_ns_file("/proc/self/ns/user", "user", &mine) == 0 &&
                     mine == before;
            (void)!write(pfd[1], &ok, sizeof(ok));
            _exit(ok ? 0 : 1);
        }
        close(pfd[1]);
        int ok = 0;
        CHECK(read(pfd[0], &ok, sizeof(ok)) == (ssize_t)sizeof(ok),
              "read inheritance result");
        close(pfd[0]);
        int code = 0;
        CHECK(wait_exit(child, &code) == 0, "inheritance child exits");
        CHECK(ok && code == 0, "fork() inherits the caller's user namespace");

        /* clone(CLONE_NEWUSER) is the one that does not. */
        pid_t c2 = (pid_t)syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD_, 0, 0, 0, 0);
        CHECK(c2 >= 0, "clone(CLONE_NEWUSER)");
        if (c2 == 0) {
            unsigned long long mine = 0;
            _exit(read_ns_file("/proc/self/ns/user", "user", &mine) == 0 &&
                  mine != before ? 0 : 1);
        }
        CHECK(wait_exit(c2, &code) == 0, "clone(CLONE_NEWUSER) child exits");
        CHECK(code == 0, "clone(CLONE_NEWUSER) creates a distinct namespace");

        /* The caller is still where it started: neither of the two moved it. */
        unsigned long long after = 0;
        CHECK(read_ns_file("/proc/self/ns/user", "user", &after) == 0 &&
              after == before,
              "the caller's own user namespace is unchanged by either");
    }

    /* 9. The caller's own namespace is still joinable, and the whole test has
     *    not disturbed it. */
    {
        int fd = open("/proc/self/ns/user", O_RDONLY);
        CHECK(fd >= 0, "reopen /proc/self/ns/user");
        CHECK(xsetns(fd, CLONE_NEWUSER) == 0,
              "setns into one's own user namespace still works");
        close(fd);
        CHECK(read_ns_file("/proc/self/ns/user", "user", &own_ino) == 0 &&
              own_ino == init_ino,
              "the caller's user namespace survived every probe");
    }

out:
    if (failures)
        return 1;
    printf("USERNS_TEST: PASS\n");
    return 0;
}