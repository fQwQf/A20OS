#include "syscall_internal.h"

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

int64_t sys_sendmsg(int fd, const void *msg, int flags)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (copy_from_user(&mh, msg, sizeof(mh)) < 0) return -EFAULT;
    if (mh.msg_iovlen < 0) return -EINVAL;
    if (mh.msg_iovlen == 0) return 0;
    struct iovec_local { void *base; size_t len; } iov;
    if (copy_from_user(&iov, mh.msg_iov, sizeof(iov)) < 0) return -EFAULT;
    return sys_sendto(fd, iov.base, iov.len, flags, mh.msg_name, mh.msg_namelen);
}

int64_t sys_recvmsg(int fd, void *msg, int flags)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (copy_from_user(&mh, msg, sizeof(mh)) < 0) return -EFAULT;
    if (mh.msg_iovlen < 0) return -EINVAL;
    if (mh.msg_iovlen == 0) return 0;
    struct iovec_local { void *base; size_t len; } iov;
    if (copy_from_user(&iov, mh.msg_iov, sizeof(iov)) < 0) return -EFAULT;
    int64_t r = sys_recvfrom(fd, iov.base, iov.len, flags, mh.msg_name, &((socket_msghdr_t *)msg)->msg_namelen);
    if (r >= 0) {
        mh.msg_flags = 0;
        copy_to_user(&((socket_msghdr_t *)msg)->msg_flags, &mh.msg_flags, sizeof(mh.msg_flags));
    }
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
        int64_t r = sys_sendmsg(fd, &((socket_mmsghdr_t *)mmsg)[sent].msg_hdr, flags);
        if (r < 0) return sent ? (int64_t)sent : r;
        one.msg_len = (unsigned)r;
        copy_to_user(&((socket_mmsghdr_t *)mmsg)[sent].msg_len, &one.msg_len, sizeof(one.msg_len));
    }
    return sent;
}

int64_t sys_recvmmsg(int fd, void *mmsg, unsigned vlen, int flags, void *timeout)
{
    (void)timeout;
    if (!mmsg) return -EFAULT;
    if (vlen > 1024) vlen = 1024;
    unsigned recvd = 0;
    for (; recvd < vlen; recvd++) {
        int64_t r = sys_recvmsg(fd, &((socket_mmsghdr_t *)mmsg)[recvd].msg_hdr, flags);
        if (r < 0) return recvd ? (int64_t)recvd : r;
        unsigned len = (unsigned)r;
        copy_to_user(&((socket_mmsghdr_t *)mmsg)[recvd].msg_len, &len, sizeof(len));
        if (r == 0) break;
    }
    return recvd;
}
