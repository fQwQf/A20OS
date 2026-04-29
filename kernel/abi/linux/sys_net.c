#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

static int copy_sockaddr_from_user(uint8_t storage[NET_SOCKADDR_MAX],
                                   const void *uaddr, size_t addrlen) {
    if (!uaddr || addrlen == 0) return -EINVAL;
    if (addrlen > NET_SOCKADDR_MAX) return -EINVAL;
    if (copy_from_user(storage, uaddr, addrlen) < 0) return -EFAULT;
    return 0;
}

static int copy_sockaddr_len_from_user(void *uaddrlen, size_t *len) {
    uint32_t v;
    if (!uaddrlen || !len) return -EFAULT;
    if (copy_from_user(&v, uaddrlen, sizeof(v)) < 0) return -EFAULT;
    if (v > NET_SOCKADDR_MAX) v = NET_SOCKADDR_MAX;
    *len = v;
    return 0;
}

static int copy_sockaddr_len_to_user(void *uaddrlen, size_t len) {
    uint32_t v = (uint32_t)len;
    if (!uaddrlen) return -EFAULT;
    return copy_to_user(uaddrlen, &v, sizeof(v)) < 0 ? -EFAULT : 0;
}

int64_t sys_socket(int domain, int type, int protocol) {
    int gfd = net_socket_create(domain, type, protocol);
    if (gfd < 0) return gfd;
    task_t *t = proc_current();
    return fdtable_install(t, gfd, type);
}

int64_t sys_socketpair(int domain, int type, int protocol, int *sv) {
    if (!sv) return -EFAULT;
    int gfds[2];
    int r = net_socketpair_create(domain, type, protocol, gfds);
    if (r < 0) return r;
    task_t *t = proc_current();
    int l0 = fdtable_install(t, gfds[0], 0);
    if (l0 < 0) {
        vfs_close(gfds[1]);
        return l0;
    }
    int l1 = fdtable_install(t, gfds[1], 0);
    if (l1 < 0) {
        fdtable_close(t, l0);
        return l1;
    }
    int out[2] = { l0, l1 };
    if (copy_to_user(sv, out, sizeof(out)) < 0) {
        fdtable_close(t, l0);
        fdtable_close(t, l1);
        return -EFAULT;
    }
    return 0;
}

int64_t sys_bind(int fd, const void *addr, size_t addrlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    int r = copy_sockaddr_from_user(kaddr, addr, addrlen);
    if (r < 0) return r;
    return net_bind((int)gfd, kaddr, addrlen);
}

int64_t sys_connect(int fd, const void *addr, size_t addrlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    int r = copy_sockaddr_from_user(kaddr, addr, addrlen);
    if (r < 0) return r;
    return net_connect((int)gfd, kaddr, addrlen);
}

int64_t sys_listen(int fd, int backlog) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    return net_listen((int)gfd, backlog);
}

int64_t sys_accept4(int fd, void *addr, void *addrlen, int flags) {
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    int is_path = vf && (vf->flags & O_PATH);
    vfs_put_file_ref((int)gfd, vf);
    if (is_path)
        return -EBADF;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = NET_SOCKADDR_MAX;
    if (addr && addrlen) {
        int r = copy_sockaddr_len_from_user(addrlen, &klen);
        if (r < 0) return r;
    }
    int new_gfd = net_accept((int)gfd, kaddr, &klen, flags);
    if (new_gfd < 0) return new_gfd;
    if (flags & SOCK_NONBLOCK)
        net_set_nonblock(new_gfd, 1);
    int lfd = fdtable_install_current( new_gfd, flags);
    if (lfd < 0) {
        vfs_close(new_gfd);
        return lfd;
    }
    if (addr && addrlen) {
        if (copy_to_user(addr, kaddr, klen) < 0) {
            sys_close(lfd);
            return -EFAULT;
        }
        int r = copy_sockaddr_len_to_user(addrlen, klen);
        if (r < 0) {
            sys_close(lfd);
            return r;
        }
    }
    return lfd;
}

int64_t sys_accept(int fd, void *addr, void *addrlen) {
    return sys_accept4(fd, addr, addrlen, 0);
}

int64_t sys_getsockname(int fd, void *addr, void *addrlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen;
    int r = copy_sockaddr_len_from_user(addrlen, &klen);
    if (r < 0) return r;
    r = net_getsockname((int)gfd, kaddr, &klen);
    if (r < 0) return r;
    if (copy_to_user(addr, kaddr, klen) < 0) return -EFAULT;
    return copy_sockaddr_len_to_user(addrlen, klen);
}

int64_t sys_getpeername(int fd, void *addr, void *addrlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen;
    int r = copy_sockaddr_len_from_user(addrlen, &klen);
    if (r < 0) return r;
    r = net_getpeername((int)gfd, kaddr, &klen);
    if (r < 0) return r;
    if (copy_to_user(addr, kaddr, klen) < 0) return -EFAULT;
    return copy_sockaddr_len_to_user(addrlen, klen);
}

int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                   const void *addr, size_t addrlen) {
    if (!buf && len) return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    if (len > 65535) return -EMSGSIZE;
    uint8_t *kbuf = (uint8_t *)proc_scratch_buffer(len ? len : 1);
    if (!kbuf) return -ENOMEM;
    if (len && copy_from_user(kbuf, buf, len) < 0) return -EFAULT;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    const void *ka = NULL;
    if (addr) {
        int r = copy_sockaddr_from_user(kaddr, addr, addrlen);
        if (r < 0) return r;
        ka = kaddr;
    }
    int r = net_sendto((int)gfd, kbuf, len, flags, ka, addrlen);
    return r;
}

int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags,
                     void *addr, void *addrlen) {
    if (!buf && len) return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    if (len > 65535) return -EMSGSIZE;
    uint8_t *kbuf = (uint8_t *)proc_scratch_buffer(len ? len : 1);
    if (!kbuf) return -ENOMEM;
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = NET_SOCKADDR_MAX;
    if (addr && addrlen) {
        int r = copy_sockaddr_len_from_user(addrlen, &klen);
        if (r < 0) return r;
    }
    int r = net_recvfrom((int)gfd, kbuf, len, flags,
                         addr ? kaddr : NULL, addr ? &klen : NULL);
    if (r >= 0 && copy_to_user(buf, kbuf, (size_t)r) < 0) {
        return -EFAULT;
    }
    if (r >= 0 && addr && addrlen) {
        if (copy_to_user(addr, kaddr, klen) < 0) return -EFAULT;
        int lr = copy_sockaddr_len_to_user(addrlen, klen);
        if (lr < 0) return lr;
    }
    return r;
}

int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, size_t optlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kopt[256];
    if (optlen > sizeof(kopt)) return -EINVAL;
    if (optlen && copy_from_user(kopt, optval, optlen) < 0) return -EFAULT;
    return net_setsockopt((int)gfd, level, optname, kopt, optlen);
}

int64_t sys_getsockopt(int fd, int level, int optname, void *optval, void *optlen) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint8_t kopt[64];
    size_t klen;
    int r = copy_sockaddr_len_from_user(optlen, &klen);
    if (r < 0) return r;
    if (klen > sizeof(kopt)) klen = sizeof(kopt);
    r = net_getsockopt((int)gfd, level, optname, kopt, &klen);
    if (r < 0) return r;
    if (copy_to_user(optval, kopt, klen) < 0) return -EFAULT;
    return copy_sockaddr_len_to_user(optlen, klen);
}

int64_t sys_shutdown(int fd, int how) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    return net_shutdown((int)gfd, how);
}
