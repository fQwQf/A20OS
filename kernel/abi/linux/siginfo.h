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

#endif
