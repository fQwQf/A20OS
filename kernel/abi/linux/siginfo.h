#ifndef A20_LINUX_SIGINFO_H
#define A20_LINUX_SIGINFO_H

/* Linux userspace siginfo_t layout, matching musl's public ABI shape. */
typedef struct {
    int signo;
    int error;
    int code;
    union {
        char pad[128 - 2 * sizeof(int) - sizeof(long)];
        struct {
            union {
                struct { int pid; unsigned int uid; } piduid;
                struct { int timerid; int overrun; } timer;
            } first;
            union {
                union { int integer; void *pointer; } value;
                struct { int status; long utime; long stime; } sigchld;
            } second;
        } common;
        struct { void *addr; short addr_lsb; } fault;
        struct { long band; int fd; } poll;
        struct { void *call_addr; int syscall; unsigned int arch; } sigsys;
    } fields;
} linux_siginfo_t;

STATIC_ASSERT(sizeof(linux_siginfo_t) == 128, linux_siginfo_size);
STATIC_ASSERT(offsetof(linux_siginfo_t, fields.common.first.piduid.pid) ==
              (sizeof(long) == 8 ? 16 : 12), linux_siginfo_pid_offset);
STATIC_ASSERT(offsetof(linux_siginfo_t, fields.common.first.piduid.uid) ==
              (sizeof(long) == 8 ? 20 : 16), linux_siginfo_uid_offset);
STATIC_ASSERT(offsetof(linux_siginfo_t, fields.common.second.sigchld.status) ==
              (sizeof(long) == 8 ? 24 : 20), linux_siginfo_status_offset);

#endif
