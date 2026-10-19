#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/*
 * Guest-side acceptance for the PID id space.
 *
 * The observable surface is /proc/sys/kernel/pid_max and
 * /proc/sys/kernel/pidmap (pid_max, next_pid, used).  The trace deliberately
 * prints booleans and deltas rather than raw pids: pid numbers assigned during
 * an SMP boot are not a stable input, while the allocator's own invariants
 * (a pid is reserved exactly once, is released exactly once, and the cursor
 * advances) are.  The C and Rust kernels must produce the same trace.
 */

#define PID_MAX_DEFAULT 32768
#define PID_MAX_LIMIT 4194304

struct pidmap {
    int pid_max;
    int next_pid;
    int used;
};

static int fail(const char *what, long value) {
    printf("PID_STATE_TEST: FAIL %s value=%ld errno=%d\n", what, value, errno);
    return 1;
}

static int read_pid_max(void) {
    int fd = open("/proc/sys/kernel/pid_max", O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static int write_pid_max(const char *text) {
    int fd = open("/proc/sys/kernel/pid_max", O_WRONLY);
    if (fd < 0)
        return -errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = errno;
    close(fd);
    return n < 0 ? -err : 0;
}

static int read_pidmap(struct pidmap *out) {
    int fd = open("/proc/sys/kernel/pidmap", O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += (size_t)n;
        if (strstr(buf, "used:") != NULL)
            break;
    }
    close(fd);
    if (total == 0)
        return -1;
    buf[total] = '\0';
    out->pid_max = 0;
    out->next_pid = 0;
    out->used = 0;
    /* "pid_max: N\nnext_pid: N\nused: N\npids:..." */
    if (sscanf(buf, "pid_max: %d\nnext_pid: %d\nused: %d", &out->pid_max,
               &out->next_pid, &out->used) != 3)
        return -1;
    return 0;
}

/* Fork `count` children that all stay alive until told to exit, so their pids
 * are simultaneously reserved.  Reports whether the ids are distinct and in
 * range, and the used delta. */
static int hold_children(int count, int *distinct, int *in_range,
                         int *used_delta) {
    int pipes[2];
    if (pipe(pipes) != 0)
        return -1;
    struct pidmap before, during;
    if (read_pidmap(&before) != 0)
        return -1;

    pid_t kids[16];
    if (count > (int)(sizeof(kids) / sizeof(kids[0])))
        return -1;
    for (int i = 0; i < count; i++) {
        kids[i] = fork();
        if (kids[i] < 0)
            return -1;
        if (kids[i] == 0) {
            /* Wait for the parent to release us.  The child must drop the
             * write end, or its own copy keeps the pipe from ever reaching
             * EOF and every sibling would block forever. */
            close(pipes[1]);
            char byte;
            ssize_t ignored = read(pipes[0], &byte, 1);
            (void)ignored;
            _exit(0);
        }
    }

    *distinct = 1;
    *in_range = 1;
    for (int i = 0; i < count; i++) {
        if (kids[i] < 1 || kids[i] > before.pid_max)
            *in_range = 0;
        for (int j = i + 1; j < count; j++) {
            if (kids[i] == kids[j])
                *distinct = 0;
        }
    }

    if (read_pidmap(&during) != 0)
        return -1;
    *used_delta = during.used - before.used;

    close(pipes[1]);
    for (int i = 0; i < count; i++) {
        int status = 0;
        if (waitpid(kids[i], &status, 0) != kids[i])
            return -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return -1;
    }
    return 0;
}

int main(void) {
    printf("PID_STATE_TEST: start\n");

    int pid_max = read_pid_max();
    if (pid_max != PID_MAX_DEFAULT)
        return fail("initial-pid-max", pid_max);
    printf("PID_STATE_TEST: initial-pid-max PASS value=%d\n", pid_max);

    struct pidmap base;
    if (read_pidmap(&base) != 0)
        return fail("pidmap-read", 0);
    if (base.pid_max != PID_MAX_DEFAULT || base.next_pid < 1 || base.used < 1)
        return fail("pidmap-fields", base.used);
    printf("PID_STATE_TEST: pidmap-fields PASS\n");

    /* getpid is stable and, in a single-threaded task, equals gettid. */
    pid_t a = getpid();
    pid_t b = getpid();
    if (a != b || a < 1)
        return fail("getpid-stable", a);
    if (syscall(SYS_gettid) != a)
        return fail("gettid-matches", a);
    printf("PID_STATE_TEST: self-id PASS\n");

    /* Eight live children hold eight distinct, in-range ids. */
    int distinct = 0, in_range = 0, used_delta = 0;
    if (hold_children(8, &distinct, &in_range, &used_delta) != 0)
        return fail("hold-children", 0);
    if (!distinct)
        return fail("children-distinct", 0);
    if (!in_range)
        return fail("children-in-range", 0);
    if (used_delta != 8)
        return fail("children-used-delta", used_delta);
    printf("PID_STATE_TEST: simultaneous-ids PASS delta=%d\n", used_delta);

    /* Reaping releases every reservation. */
    struct pidmap after;
    if (read_pidmap(&after) != 0)
        return fail("pidmap-after", 0);
    if (after.used != base.used)
        return fail("used-after-reap", after.used - base.used);
    printf("PID_STATE_TEST: reap-releases PASS\n");

    /* The cursor advanced across the allocations above. */
    if (after.next_pid == base.next_pid)
        return fail("next-pid-advanced", after.next_pid);
    printf("PID_STATE_TEST: cursor-advances PASS\n");

    /* Repeated fork/reap cycles keep the reservation count flat. */
    for (int round = 0; round < 200; round++) {
        pid_t child = fork();
        if (child < 0)
            return fail("cycle-fork", round);
        if (child == 0)
            _exit(0);
        int status = 0;
        if (waitpid(child, &status, 0) != child)
            return fail("cycle-wait", round);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("cycle-status", round);
    }
    struct pidmap after_cycles;
    if (read_pidmap(&after_cycles) != 0)
        return fail("pidmap-cycles", 0);
    if (after_cycles.used != base.used)
        return fail("used-after-cycles", after_cycles.used - base.used);
    printf("PID_STATE_TEST: fork-reap-cycles PASS rounds=200\n");

    /* pid_max is writable and read back; allocation stays inside it. */
    if (write_pid_max("4096\n") != 0)
        return fail("pid-max-write", errno);
    if (read_pid_max() != 4096)
        return fail("pid-max-readback", read_pid_max());
    if (read_pidmap(&after_cycles) != 0)
        return fail("pidmap-after-write", 0);
    if (after_cycles.pid_max != 4096)
        return fail("pidmap-max-after-write", after_cycles.pid_max);
    if (hold_children(4, &distinct, &in_range, &used_delta) != 0)
        return fail("bounded-hold", 0);
    if (!distinct || !in_range)
        return fail("bounded-ids", in_range);
    if (write_pid_max("32768\n") != 0)
        return fail("pid-max-restore", errno);
    if (read_pid_max() != PID_MAX_DEFAULT)
        return fail("pid-max-restored", read_pid_max());
    printf("PID_STATE_TEST: pid-max-window PASS\n");

    /* Out-of-range values are rejected and change nothing. */
    errno = 0;
    if (write_pid_max("0\n") == 0 || errno != EINVAL)
        return fail("pid-max-zero", errno);
    errno = 0;
    if (write_pid_max("4194305\n") == 0 || errno != EINVAL)
        return fail("pid-max-too-large", errno);
    if (read_pid_max() != PID_MAX_DEFAULT)
        return fail("pid-max-unchanged", read_pid_max());
    printf("PID_STATE_TEST: pid-max-refusals PASS\n");

    printf("PID_STATE_TEST: PASS\n");
    return 0;
}
