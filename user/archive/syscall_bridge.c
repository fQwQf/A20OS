/*
 * A20OS musl port — syscall semantic bridge.
 *
 * Translates Linux/POSIX syscall conventions to A20 Native ABI calls.
 * musl calls __syscall_* with Linux-style arguments; this layer
 * converts them to A20 handle-based operations.
 */
#include <stdint.h>
#include <stddef.h>

#define A20_OK 0

#include "syscall.h"

long __a20_bridge_exit(int code)
{
    __syscall1(0x0200, code);
    for (;;) {}
}

long __a20_bridge_read(int fd, void *buf, size_t count)
{
    return __syscall3(0x0401, fd, (long)buf, count);
}

long __a20_bridge_write(int fd, const void *buf, size_t count)
{
    return __syscall3(0x0402, fd, (long)buf, count);
}

long __a20_bridge_open(const char *path, int flags, int mode)
{
    return __syscall3(0x0400, (long)path, flags, mode);
}

long __a20_bridge_close(int fd)
{
    return __syscall1(0x0100, fd);
}

long __a20_bridge_lseek(int fd, long offset, int whence)
{
    return __syscall3(0x0105, fd, offset, whence);
}

long __a20_bridge_mmap(void *addr, size_t len, int prot, int flags,
                       int fd, long offset)
{
    return __syscall6(0x0303, (long)addr, len, prot, flags, fd, offset);
}

long __a20_bridge_munmap(void *addr, size_t len)
{
    return __syscall2(0x0301, (long)addr, len);
}

long __a20_bridge_mprotect(void *addr, size_t len, int prot)
{
    return __syscall3(0x0302, (long)addr, len, prot);
}

long __a20_bridge_brk(long addr)
{
    return __syscall1(0x0300, addr);
}

long __a20_bridge_clock_gettime(int clk, void *ts)
{
    return __syscall2(0x0700, clk, (long)ts);
}

long __a20_bridge_nanosleep(const void *req, void *rem)
{
    return __syscall2(0x0207, (long)req, (long)rem);
}

long __a20_bridge_getpid(void)
{
    return __syscall0(0x0204);
}

long __a20_bridge_kill(int pid, int sig)
{
    return __syscall2(0x0203, pid, sig);
}

long __a20_bridge_socket(int domain, int type, int proto)
{
    return __syscall3(0x0600, domain, type, proto);
}

long __a20_bridge_sched_yield(void)
{
    return __syscall0(0x0208);
}
