/* Real F-Stack socket calls, executed only by the owning BSD worker.
 * This file uses BSD/private headers and is namespace-prefixed at link time. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include "ff_api.h"
#include "ff_host_interface.h"

extern int port_last_error(void);
extern void ff_hardclock(void);

/* F-Stack's public wrapper accepts Linux socket constants but reports BSD
 * errno. Keep the conversion here, before values reach A20OS's Linux ABI. */
static int linux_errno(int error)
{
    switch (error) {
    case 0: return 0;
    case EPERM: return 1;
    case ENOENT: return 2;
    case ESRCH: return 3;
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case E2BIG: return 7;
    case ENOEXEC: return 8;
    case EBADF: return 9;
    case ECHILD: return 10;
    case EDEADLK: return 35;
    case ENOMEM: return 12;
    case EACCES: return 13;
    case EFAULT: return 14;
    case ENOTBLK: return 15;
    case EBUSY: return 16;
    case EEXIST: return 17;
    case EXDEV: return 18;
    case ENODEV: return 19;
    case ENOTDIR: return 20;
    case EISDIR: return 21;
    case EINVAL: return 22;
    case ENFILE: return 23;
    case EMFILE: return 24;
    case ENOTTY: return 25;
    case ETXTBSY: return 26;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case ESPIPE: return 29;
    case EROFS: return 30;
    case EMLINK: return 31;
    case EPIPE: return 32;
    case EDOM: return 33;
    case ERANGE: return 34;
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case EOPNOTSUPP: return 95;
    case EPFNOSUPPORT: return 96;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    case ENOSYS: return 38;
    case ENOTEMPTY: return 39;
    case ELOOP: return 40;
    case ENAMETOOLONG: return 36;
    case EOVERFLOW: return 75;
    case ECANCELED: return 125;
    default: return 5; /* EIO: never accidentally expose a BSD errno number. */
    }
}

static int result(int value)
{
    return value < 0 ? -linux_errno(port_last_error()) : value;
}

int port_socket_create(int domain, int type, int protocol)
{
    /* Linux SOCK_NONBLOCK. All protocol waits are owned by A20OS's frontend;
     * the single BSD worker must always remain runnable. */
    return result(ff_socket(domain, type | 0x800, protocol));
}

int port_socket_bind(int fd, const void *address, size_t length)
{
    return result(ff_bind(fd, (const struct linux_sockaddr *)address,
                          (socklen_t)length));
}

int port_socket_connect(int fd, const void *address, size_t length)
{
    return result(ff_connect(fd, (const struct linux_sockaddr *)address,
                             (socklen_t)length));
}

int port_socket_listen(int fd, int backlog)
{
    return result(ff_listen(fd, backlog));
}

int port_socket_accept(int fd, void *address, size_t *length)
{
    socklen_t local = length ? (socklen_t)*length : 0;
    int fd_new = ff_accept(fd, (struct linux_sockaddr *)address,
                           length ? &local : NULL);
    if (length) *length = local;
    if (fd_new < 0) return -linux_errno(port_last_error());
    int nonblocking = 1;
    if (ff_ioctl(fd_new, 0x5421, &nonblocking) < 0) {
        int error = linux_errno(port_last_error());
        ff_close(fd_new);
        return -error;
    }
    return fd_new;
}

int port_socket_close(int fd) { return result(ff_close(fd)); }

int port_socket_shutdown(int fd, int receive, int transmit)
{
    int how = receive ? (transmit ? SHUT_RDWR : SHUT_RD) :
                        (transmit ? SHUT_WR : -1);
    if (how < 0) return -22;
    /* ff_shutdown uses the Linux-facing wrapper and translates these values. */
    return result(ff_shutdown(fd, how));
}

int port_socket_send(int fd, const void *buffer, size_t length, int flags,
                     const void *address, size_t address_length)
{
    ssize_t sent = ff_sendto(fd, buffer, length, flags,
                             (const struct linux_sockaddr *)address,
                             (socklen_t)address_length);
    return sent < 0 ? -linux_errno(port_last_error()) : (int)sent;
}

int port_socket_recv(int fd, void *buffer, size_t length, int flags,
                     void *address, size_t *address_length)
{
    socklen_t local = address_length ? (socklen_t)*address_length : 0;
    ssize_t received = ff_recvfrom(fd, buffer, length, flags,
                                   (struct linux_sockaddr *)address,
                                   address_length ? &local : NULL);
    if (address_length) *address_length = local;
    return received < 0 ? -linux_errno(port_last_error()) : (int)received;
}

int port_socket_error(int fd)
{
    int error = 0;
    socklen_t length = sizeof(error);
    /* The Linux-facing wrapper's level conversion is ambiguous for
     * SOL_SOCKET; use the FreeBSD-native entry with BSD constants here. */
    if (ff_getsockopt_freebsd(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        return -linux_errno(port_last_error());
    return linux_errno(error);
}

int port_socket_poll(int fd, int events)
{
    struct pollfd pfd = {.fd = fd, .events = (short)events, .revents = 0};
    int ready = ff_poll(&pfd, 1, 0);
    if (ready < 0) return -linux_errno(port_last_error());
    return ready ? pfd.revents : 0;
}

int port_socket_getpeername(int fd, void *address, size_t *length)
{
    socklen_t local = length ? (socklen_t)*length : 0;
    int rc = ff_getpeername(fd, (struct linux_sockaddr *)address,
                            length ? &local : NULL);
    if (length) *length = local;
    return result(rc);
}

int port_socket_getsockname(int fd, void *address, size_t *length)
{
    socklen_t local = length ? (socklen_t)*length : 0;
    int rc = ff_getsockname(fd, (struct linux_sockaddr *)address,
                            length ? &local : NULL);
    if (length) *length = local;
    return result(rc);
}

int port_socket_set_int(int fd, int level, int option, int value)
{
    return result(ff_setsockopt(fd, level, option, &value, sizeof(value)));
}

void port_protocol_progress(void) { ff_swi_net_excute(); }
void port_hardclock(void) { ff_hardclock(); }
