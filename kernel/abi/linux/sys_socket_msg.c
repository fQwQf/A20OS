#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "net/socket_internal.h"

typedef struct {
    void *msg_name;
    uint32_t msg_namelen;
    uint32_t __pad1;
    void *msg_iov;
    int msg_iovlen;
    int __pad_iovlen;
    void *msg_control;
    uint32_t msg_controllen;
    uint32_t __pad_controllen;
    int msg_flags;
} socket_msghdr_t;

typedef struct {
    socket_msghdr_t msg_hdr;
    unsigned msg_len;
    unsigned __pad;
} socket_mmsghdr_t;

typedef struct {
    void *base;
    size_t len;
} socket_iovec_t;

static uint64_t socket_timespec_to_ticks(const void *timeout)
{
    if (!timeout)
        return 0;
    int64_t ts[2];
    if (copy_from_user(ts, timeout, sizeof(ts)) < 0)
        return 0;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL)
        return 0;
    uint64_t ticks = (uint64_t)ts[0] * TICKS_PER_SEC +
                     (uint64_t)ts[1] * TICKS_PER_SEC / 1000000000ULL;
    return ticks ? ticks : 1;
}

static int64_t sys_sendmsg_from_msghdr(int fd, const socket_msghdr_t *mh,
                                       int flags)
{
    if (!mh) return -EFAULT;
    if (mh->msg_iovlen < 0) return -EINVAL;
    if (mh->msg_iovlen == 0) return 0;
    if (mh->msg_iovlen > 1024) return -EINVAL;

    size_t iov_bytes = (size_t)mh->msg_iovlen * sizeof(socket_iovec_t);
    socket_iovec_t *iov = (socket_iovec_t *)kmalloc(iov_bytes);
    if (!iov) return -ENOMEM;
    if (copy_from_user(iov, mh->msg_iov, iov_bytes) < 0) {
        kfree(iov);
        return -EFAULT;
    }

    size_t total = 0;
    for (int i = 0; i < mh->msg_iovlen; i++) {
        total += iov[i].len;
    }
    if (total == 0) {
        kfree(iov);
        return sys_sendto(fd, "", 0, flags, mh->msg_name, mh->msg_namelen);
    }

    uint8_t *buf = (uint8_t *)proc_scratch_buffer(total);
    if (!buf) {
        kfree(iov);
        return -ENOMEM;
    }
    size_t off = 0;
    for (int i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len == 0)
            continue;
        if (!iov[i].base) {
            kfree(iov);
            return -EFAULT;
        }
        if (copy_from_user(buf + off, iov[i].base, iov[i].len) < 0) {
            kfree(iov);
            return -EFAULT;
        }
        off += iov[i].len;
    }
    kfree(iov);

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;

    uint8_t kaddr[NET_SOCKADDR_MAX];
    const void *ka = NULL;
    if (mh->msg_name) {
        if (mh->msg_namelen == 0 || mh->msg_namelen > NET_SOCKADDR_MAX)
            return -EINVAL;
        if (copy_from_user(kaddr, mh->msg_name, mh->msg_namelen) < 0)
            return -EFAULT;
        ka = kaddr;
    }
    return net_sendto((int)gfd, buf, total, flags, ka, mh->msg_namelen);
}

int64_t sys_sendmsg(int fd, const void *msg, int flags)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (copy_from_user(&mh, msg, sizeof(mh)) < 0) return -EFAULT;
    return sys_sendmsg_from_msghdr(fd, &mh, flags);
}

int64_t sys_recvmsg(int fd, void *msg, int flags)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (copy_from_user(&mh, msg, sizeof(mh)) < 0) return -EFAULT;
    if (mh.msg_iovlen < 0) return -EINVAL;
    if (mh.msg_iovlen == 0) return 0;
    if (mh.msg_iovlen > 1024) return -EINVAL;

    size_t iov_bytes = (size_t)mh.msg_iovlen * sizeof(socket_iovec_t);
    socket_iovec_t *iov = (socket_iovec_t *)kmalloc(iov_bytes);
    if (!iov) return -ENOMEM;
    if (copy_from_user(iov, mh.msg_iov, iov_bytes) < 0) {
        kfree(iov);
        return -EFAULT;
    }

    size_t total = 0;
    for (int i = 0; i < mh.msg_iovlen; i++) {
        total += iov[i].len;
    }
    uint8_t *buf = (uint8_t *)proc_scratch_buffer(total ? total : 1);
    if (!buf) {
        kfree(iov);
        return -ENOMEM;
    }

    int64_t r = sys_recvfrom(fd, buf, total, flags, mh.msg_name,
                             &((socket_msghdr_t *)msg)->msg_namelen);
    if (r >= 0) {
        size_t copied = 0;
        for (int i = 0; i < mh.msg_iovlen && copied < (size_t)r; i++) {
            size_t n = (size_t)r - copied;
            if (n > iov[i].len)
                n = iov[i].len;
            if (n && (!iov[i].base ||
                      copy_to_user(iov[i].base, buf + copied, n) < 0)) {
                kfree(iov);
                return -EFAULT;
            }
            copied += n;
        }
        mh.msg_flags = 0;
        copy_to_user(&((socket_msghdr_t *)msg)->msg_flags, &mh.msg_flags, sizeof(mh.msg_flags));
    }
    kfree(iov);
    return r;
}

int64_t sys_sendmmsg(int fd, void *mmsg, unsigned vlen, int flags)
{
    if (!mmsg) return -EFAULT;
    if (vlen > 1024) vlen = 1024;
    unsigned sent = 0;
    for (; sent < vlen; sent++) {
        socket_mmsghdr_t one;
        void *up = (char *)mmsg + (size_t)sent * sizeof(one);
        if (copy_from_user(&one, up, sizeof(one)) < 0) return sent ? (int64_t)sent : -EFAULT;
        int64_t r = sys_sendmsg_from_msghdr(fd, &one.msg_hdr, flags);
        if (r < 0) return sent ? (int64_t)sent : r;
        one.msg_len = (unsigned)r;
        copy_to_user(&((socket_mmsghdr_t *)mmsg)[sent].msg_len, &one.msg_len, sizeof(one.msg_len));
    }
    return sent;
}

int64_t sys_recvmmsg(int fd, void *mmsg, unsigned vlen, int flags, void *timeout)
{
    if (!mmsg) return -EFAULT;
    if (vlen > 1024) vlen = 1024;
    uint64_t deadline = 0;
    uint64_t old_timeout = 0;
    net_socket_t *sock = NULL;
    if (timeout) {
        uint64_t ticks = socket_timespec_to_ticks(timeout);
        if (ticks == 0)
            return -EINVAL;
        deadline = timer_get_ticks() + ticks;
        int64_t gfd = fdtable_get_current(fd);
        if (gfd < 0)
            return gfd;
        sock = net_socket_from_file((int)gfd);
        if (!sock)
            return -ENOTSOCK;
        old_timeout = sock->recv_timeout_ticks;
    }
    unsigned recvd = 0;
    for (; recvd < vlen; recvd++) {
        int one_flags = flags;
        if (recvd > 0)
            one_flags |= MSG_DONTWAIT;
        if (sock && recvd == 0) {
            uint64_t now = timer_get_ticks();
            sock->recv_timeout_ticks = now >= deadline ? 1 : deadline - now;
        }
        int64_t r = sys_recvmsg(fd, &((socket_mmsghdr_t *)mmsg)[recvd].msg_hdr, one_flags);
        if (r < 0) {
            if (sock)
                sock->recv_timeout_ticks = old_timeout;
            int64_t out = recvd ? (int64_t)recvd : r;
            return out;
        }
        unsigned len = (unsigned)r;
        copy_to_user(&((socket_mmsghdr_t *)mmsg)[recvd].msg_len, &len, sizeof(len));
        if (r == 0) break;
        if (flags & MSG_WAITFORONE)
            flags |= MSG_DONTWAIT;
    }
    if (sock)
        sock->recv_timeout_ticks = old_timeout;
    return recvd;
}
