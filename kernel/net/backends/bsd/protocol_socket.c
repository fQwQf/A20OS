/* Real F-Stack socket calls, executed only by the owning BSD worker.
 * This file uses BSD/private headers and is namespace-prefixed at link time. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <net/vnet.h>
#include <netinet/icmp6.h>
#include <net/if_dl.h>
#include "ff_api.h"
#include "ff_host_interface.h"
#include <a20_bsd_ipv4.h>
#include "protocol_abi.h"

extern int port_last_error(void);
extern void ff_hardclock(void);

#define A20_LINUX_AF_INET6 10

/* ff_recvmsg has a BSD prototype but consumes Linux's msghdr layout. */
struct port_linux_msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

/* F-Stack's public wrapper accepts Linux socket constants but reports BSD
 * errno. Keep the conversion here, before values reach A20OS's Linux ABI. */
int port_linux_errno(int error)
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
    return value < 0 ? -port_linux_errno(port_last_error()) : value;
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
    if (fd_new < 0) return -port_linux_errno(port_last_error());
    int nonblocking = 1;
    if (ff_ioctl(fd_new, 0x5421, &nonblocking) < 0) {
        int error = port_linux_errno(port_last_error());
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
    return sent < 0 ? -port_linux_errno(port_last_error()) : (int)sent;
}

int port_socket_recv(int fd, void *buffer, size_t length, int flags,
                     void *address, size_t *address_length)
{
    socklen_t local = address_length ? (socklen_t)*address_length : 0;
    ssize_t received = ff_recvfrom(fd, buffer, length, flags,
                                   (struct linux_sockaddr *)address,
                                   address_length ? &local : NULL);
    if (address_length) *address_length = local;
    return received < 0 ? -port_linux_errno(port_last_error()) : (int)received;
}

int port_socket_enable_pktinfo(int fd, int domain)
{
    int one = 1;
    int rc;
    if (domain == AF_INET) {
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IP, IP_RECVDSTADDR,
                                          &one, sizeof(one)));
        if (rc < 0) return rc;
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IP, IP_RECVIF,
                                          &one, sizeof(one)));
        if (rc < 0) return rc;
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IP, IP_RECVTTL,
                                          &one, sizeof(one)));
        if (rc < 0) return rc;
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IP, IP_RECVTOS,
                                          &one, sizeof(one)));
        return rc;
    }
    if (domain == A20_LINUX_AF_INET6) {
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                                          &one, sizeof(one)));
        if (rc < 0) return rc;
        rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT,
                                          &one, sizeof(one)));
        if (rc < 0) return rc;
        return result(ff_setsockopt_freebsd(fd, IPPROTO_IPV6, IPV6_RECVTCLASS,
                                            &one, sizeof(one)));
    }
    return -EAFNOSUPPORT;
}

int port_socket_bind_if4(int fd, unsigned ifindex)
{
    int option = (int)ifindex;
    return result(ff_setsockopt_freebsd(fd, IPPROTO_IP, A20_IP_BOUND_IF,
                                        &option, sizeof(option)));
}

int port_socket_bind_if6(int fd, unsigned ifindex)
{
    if (ifindex) {
        int one = 1;
        int rc = result(ff_setsockopt_freebsd(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                                              &one, sizeof(one)));
        if (rc < 0)
            return rc;
    }
    struct in6_pktinfo info = {.ipi6_ifindex = ifindex};
    return result(ff_setsockopt_freebsd(fd, IPPROTO_IPV6, IPV6_PKTINFO,
                                        &info, sizeof(info)));
}

/* Map Linux option numbers explicitly at the port boundary. */
int port_socket_ipv6_option(int fd, int level, int option,
                            const void *value, size_t length)
{
    if (level == IPPROTO_ICMPV6 && option == 1) {
        struct icmp6_filter filter;
        if (length != sizeof(filter))
            return -22;
        memcpy(&filter, value, sizeof(filter));
        /* Linux stores blocked types, FreeBSD stores permitted types. */
        for (unsigned i = 0; i < sizeof(filter.icmp6_filt) / sizeof(filter.icmp6_filt[0]); ++i)
            filter.icmp6_filt[i] = ~filter.icmp6_filt[i];
        return result(ff_setsockopt_freebsd(fd, level, ICMP6_FILTER, &filter, sizeof(filter)));
    }
    int native;
    if (level == IPPROTO_IPV6) {
        switch (option) {
        case 7:  native = IPV6_CHECKSUM; break;
        case 17: native = IPV6_MULTICAST_IF; break;
        case 18: native = IPV6_MULTICAST_HOPS; break;
        case 19: native = IPV6_MULTICAST_LOOP; break;
        case 20: native = IPV6_JOIN_GROUP; break;
        case 21: native = IPV6_LEAVE_GROUP; break;
        case 67: native = IPV6_TCLASS; break;
        default: return -92;
        }
    } else {
        return -92;
    }
    return result(ff_setsockopt_freebsd(fd, level, native, value, length));
}

int port_socket_multicast(int fd, uint32_t group, uint32_t ifaddr, int join)
{
    struct ip_mreq req;
    memset(&req, 0, sizeof(req));
    memcpy(&req.imr_multiaddr, &group, 4);
    memcpy(&req.imr_interface, &ifaddr, 4);
    return result(ff_setsockopt_freebsd(
        fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
        &req, sizeof(req)));
}

int port_socket_recv_meta(int fd, void *buffer, size_t length, int flags,
                          void *address, size_t *address_length,
                          a20_bsd_pktinfo_t *info)
{
    struct iovec iov = { .iov_base = buffer, .iov_len = length };
    struct port_linux_msghdr msg;
    _Alignas(uint64_t) uint8_t control[192];
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    if (info) memset(info, 0, sizeof(*info));
    msg.msg_name = address;
    msg.msg_namelen = address_length ? (socklen_t)*address_length : 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t received = ff_recvmsg(fd, (struct msghdr *)(void *)&msg, flags);
    if (address_length) *address_length = msg.msg_namelen;
    if (received < 0) return -port_linux_errno(port_last_error());

    if (info) {
        /* ff_recvmsg() rewrites the control buffer from FreeBSD into the
         * Linux cmsg ABI before returning: the 64-bit length at offset 0 and
         * level/type at offsets 8/12.  Reading it as struct cmsghdr here sees
         * zeroed level/type because the two headers overlap differently. */
        typedef struct {
            uint64_t cmsg_len;
            int cmsg_level;
            int cmsg_type;
        } recv_cmsg_t;
        enum { RECV_CMSG_IP_TOS = 1, RECV_CMSG_IP_TTL = 2 };
        uint8_t *control = msg.msg_control;
        size_t controllen = msg.msg_controllen;
        size_t offset = 0;
        while (control && offset + sizeof(recv_cmsg_t) <= controllen) {
            recv_cmsg_t *c = (recv_cmsg_t *)(void *)(control + offset);
            if (c->cmsg_len < sizeof(*c) ||
                offset + c->cmsg_len > controllen)
                break;
            uint8_t *data = (uint8_t *)c + sizeof(*c);
            size_t payload = (size_t)c->cmsg_len - sizeof(*c);
            if (c->cmsg_level == IPPROTO_IP) {
                if (c->cmsg_type == IP_RECVDSTADDR && payload >= 4) {
                    memcpy(info->address, data, 4);
                    info->family = AF_INET;
                    info->valid = 1;
                } else if (c->cmsg_type == IP_RECVIF && payload >= 8) {
                    struct sockaddr_dl *sdl = (struct sockaddr_dl *)data;
                    info->ifindex = sdl->sdl_index;
                    info->valid = 1;
                } else if (c->cmsg_type == RECV_CMSG_IP_TTL && payload >= 4) {
                    info->hoplimit = (uint8_t)*(int *)data;
                    info->valid = 1;
                } else if (c->cmsg_type == RECV_CMSG_IP_TOS && payload >= 1) {
                    info->tclass = data[0];
                    info->valid = 1;
                }
            } else if (c->cmsg_level == IPPROTO_IPV6) {
                if (c->cmsg_type == IPV6_PKTINFO &&
                    payload >= sizeof(struct in6_pktinfo)) {
                    struct in6_pktinfo *pi =
                        (struct in6_pktinfo *)data;
                    memcpy(info->address, &pi->ipi6_addr, 16);
                    info->ifindex = pi->ipi6_ifindex;
                    info->family = A20_LINUX_AF_INET6;
                    info->valid = 1;
                } else if (c->cmsg_type == IPV6_HOPLIMIT && payload >= 4) {
                    info->hoplimit = (uint8_t)*(int *)data;
                    info->valid = 1;
                } else if (c->cmsg_type == IPV6_TCLASS && payload >= 4) {
                    info->tclass = (uint8_t)*(int *)data;
                    info->valid = 1;
                }
            }
            offset += (size_t)((c->cmsg_len + 7) & ~(uint64_t)7);
        }
    }
    return (int)received;
}

int port_socket_send_control(int fd, const void *buffer, size_t length,
                             int flags, const void *address,
                             size_t address_length, uint32_t ifindex,
                             const uint8_t source_v4[4])
{
    struct iovec iov = { .iov_base = (void *)buffer, .iov_len = length };
    struct msghdr msg;
    uint8_t control[128];
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    msg.msg_name = (void *)address;
    msg.msg_namelen = (socklen_t)address_length;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    /* ff_sendmsg() is F-Stack's Linux-facing entry point: it reads the
     * control buffer with Linux cmsghdr layout (8-byte length, level at 8,
     * type at 12, data at 16) and translates it to the FreeBSD one.  Handing
     * it a FreeBSD cmsghdr made the translator read the length from the wrong
     * offset and the send never returned.  IPv4 source selection has no
     * translation entry, so the type is the FreeBSD IP_SENDSRCADDR value and
     * the payload is copied verbatim; IPv6 uses Linux's IPV6_PKTINFO, which
     * the translator maps to the FreeBSD constant. */
    struct send_cmsg_linux {
        uint64_t cmsg_len;
        int cmsg_level;
        int cmsg_type;
    } *c = (struct send_cmsg_linux *)(void *)control;
    int family = address ? ((const struct sockaddr *)address)->sa_family
                         : AF_UNSPEC;
    if (family == AF_INET && source_v4 && (source_v4[0] || source_v4[1] ||
                                           source_v4[2] || source_v4[3])) {
        c->cmsg_level = IPPROTO_IP;
        c->cmsg_type = IP_SENDSRCADDR;
        c->cmsg_len = sizeof(*c) + sizeof(struct in_addr); /* 20 */
        memcpy(control + sizeof(*c), source_v4, sizeof(struct in_addr));
        msg.msg_controllen = sizeof(*c) + 8; /* CMSG_SPACE(4) in Linux */
    } else if (family == AF_INET6 && ifindex) {
        c->cmsg_level = IPPROTO_IPV6;
        c->cmsg_type = 50; /* Linux IPV6_PKTINFO */
        c->cmsg_len = sizeof(*c) + sizeof(struct in6_pktinfo);
        struct in6_pktinfo *pi = (struct in6_pktinfo *)(void *)(control + sizeof(*c));
        memset(pi, 0, sizeof(*pi));
        pi->ipi6_ifindex = (unsigned)ifindex;
        msg.msg_controllen = sizeof(*c) + ((sizeof(struct in6_pktinfo) + 7) & ~7u);
    } else {
        msg.msg_control = NULL;
        msg.msg_controllen = 0;
    }

    ssize_t sent = ff_sendmsg(fd, &msg, flags);
    return sent < 0 ? -port_linux_errno(port_last_error()) : (int)sent;
}

int port_socket_error(int fd)
{
    int error = 0;
    socklen_t length = sizeof(error);
    /* The Linux-facing wrapper's level conversion is ambiguous for
     * SOL_SOCKET; use the FreeBSD-native entry with BSD constants here. */
    if (ff_getsockopt_freebsd(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        return -port_linux_errno(port_last_error());
    return port_linux_errno(error);
}

int port_socket_poll(int fd, int events)
{
    struct pollfd pfd = {.fd = fd, .events = (short)events, .revents = 0};
    int ready = ff_poll(&pfd, 1, 0);
    if (ready < 0) return -port_linux_errno(port_last_error());
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

int port_socket_set_int_freebsd(int fd, int level, int option, int value)
{
    return result(ff_setsockopt_freebsd(fd, level, option, &value,
                                        sizeof(value)));
}

int port_socket_get_int_freebsd(int fd, int level, int option, int *value)
{
    socklen_t length = sizeof(*value);
    return result(ff_getsockopt_freebsd(fd, level, option, value, &length));
}

int port_socket_set_str_freebsd(int fd, int level, int option,
                                const char *value)
{
    if (!value) return -EINVAL;
    return result(ff_setsockopt_freebsd(fd, level, option, value,
                                        (socklen_t)strlen(value) + 1));
}

void port_protocol_progress(void) { ff_swi_net_excute(); }
void port_hardclock(void) { ff_hardclock(); }

/* ---- Readiness multiplexing ---------------------------------------------
 * The backend executor owns exactly one kqueue.  A socket is registered once
 * when its descriptor is created (or accepted) and removed before close, so a
 * progress pass costs one kevent() instead of one non-blocking recv() for
 * every open socket.  That is the difference between O(ready) and O(open). */
#define READINESS_BATCH 64
static int readiness_kq = -1;
static struct kevent readiness_events[READINESS_BATCH];

int port_readiness_init(void)
{
    if (readiness_kq >= 0)
        return 0;
    int kq = ff_kqueue();
    if (kq < 0)
        return -port_linux_errno(port_last_error());
    readiness_kq = kq;
    return 0;
}

int port_readiness_add(int fd, void *tag)
{
    if (fd < 0)
        return -EINVAL;
    int error = port_readiness_init();
    if (error < 0)
        return error;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, tag);
    if (ff_kevent(readiness_kq, &change, 1, NULL, 0, NULL) < 0)
        return -port_linux_errno(port_last_error());
    return 0;
}

void port_readiness_remove(int fd)
{
    if (readiness_kq < 0 || fd < 0)
        return;
    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    (void)ff_kevent(readiness_kq, &change, 1, NULL, 0, NULL);
}

int port_readiness_wait(a20_bsd_ready_t *events, int maximum)
{
    if (readiness_kq < 0 || !events || maximum <= 0)
        return 0;
    if (maximum > READINESS_BATCH)
        maximum = READINESS_BATCH;
    int count = ff_kevent(readiness_kq, NULL, 0, readiness_events, maximum,
                          NULL);
    if (count < 0) {
        int error = port_last_error();
        /* A spurious wakeup is not a socket error. */
        if (error == EINTR || error == EWOULDBLOCK || error == EAGAIN)
            return 0;
        return -port_linux_errno(error);
    }
    for (int i = 0; i < count; ++i) {
        events[i].fd = (int)readiness_events[i].ident;
        events[i].flags = (readiness_events[i].flags & EV_EOF)
                              ? A20_BSD_READY_EOF : 0u;
        events[i].tag = readiness_events[i].udata;
    }
    return count;
}
