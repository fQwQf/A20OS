#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "net/socket_internal.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "lwip/raw.h"

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

typedef struct {
    uint32_t cmsg_len;
    int __pad_cmsg_len;
    int cmsg_level;
    int cmsg_type;
} socket_cmsghdr_t;

typedef struct {
    uint8_t ipi6_addr[16];
    unsigned ipi6_ifindex;
} socket_in6_pktinfo_t;

typedef struct {
    int has_pktinfo;
    int has_hoplimit;
    int has_tclass;
    int has_2292_pktinfo;
    int has_2292_hoplimit;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    int hoplimit;
    int tclass;
} socket_recv_meta_t;

/*
 * SCM_RIGHTS fds resolved from a sendmsg control buffer.  Each entry is a
 * vfile reference (dup semantics) owned by this struct until the message
 * is enqueued or the refs are dropped.
 */
typedef struct {
    int nfiles;
    vfile_t *files[NET_SCM_MAX_FDS];
} scm_rights_t;

static size_t cmsg_align(size_t len)
{
    return (len + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
}

static size_t cmsg_space(size_t len)
{
    return cmsg_align(sizeof(socket_cmsghdr_t)) + cmsg_align(len);
}

static size_t cmsg_len(size_t len)
{
    return cmsg_align(sizeof(socket_cmsghdr_t)) + len;
}

static void *cmsg_data(socket_cmsghdr_t *cmsg)
{
    return (void *)(cmsg + 1);
}

static const void *cmsg_data_const(const socket_cmsghdr_t *cmsg)
{
    return (const void *)(cmsg + 1);
}

static socket_cmsghdr_t *cmsg_firsthdr(const socket_msghdr_t *mh)
{
    return mh->msg_controllen >= sizeof(socket_cmsghdr_t) ?
           (socket_cmsghdr_t *)mh->msg_control : NULL;
}

static socket_cmsghdr_t *cmsg_nxthdr(const socket_msghdr_t *mh,
                                     const socket_cmsghdr_t *cmsg)
{
    size_t next = (size_t)((const uint8_t *)cmsg - (const uint8_t *)mh->msg_control) +
                  cmsg_align(cmsg->cmsg_len);
    if (cmsg->cmsg_len < sizeof(socket_cmsghdr_t) ||
        next + sizeof(socket_cmsghdr_t) > mh->msg_controllen)
        return NULL;
    return (socket_cmsghdr_t *)((uint8_t *)mh->msg_control + next);
}

static int parse_send_control(const socket_msghdr_t *mh, int *ttl, int *tclass)
{
    if (!mh->msg_control || mh->msg_controllen < sizeof(socket_cmsghdr_t))
        return 0;
    socket_cmsghdr_t *cmsg = cmsg_firsthdr(mh);
    while (cmsg) {
        size_t need = cmsg_len(0);
        if (cmsg->cmsg_len < need || cmsg->cmsg_len > mh->msg_controllen)
            return -EINVAL;
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT) {
            if (cmsg->cmsg_len < cmsg_len(sizeof(int)))
                return -EINVAL;
            memcpy(ttl, cmsg_data_const(cmsg), sizeof(int));
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_TCLASS) {
            if (cmsg->cmsg_len < cmsg_len(sizeof(int)))
                return -EINVAL;
            memcpy(tclass, cmsg_data_const(cmsg), sizeof(int));
        }
        cmsg = cmsg_nxthdr(mh, cmsg);
    }
    return 0;
}

static void scm_rights_clear(scm_rights_t *scm)
{
    net_scm_drop_files(scm->files, scm->nfiles);
    scm->nfiles = 0;
}

/*
 * Resolve SCM_RIGHTS fd arrays in a send control buffer to vfile
 * references.  On error, partially resolved refs are dropped.
 */
static int parse_send_rights(const socket_msghdr_t *mh, scm_rights_t *out)
{
    out->nfiles = 0;
    if (!mh->msg_control || mh->msg_controllen < sizeof(socket_cmsghdr_t))
        return 0;
    socket_cmsghdr_t *cmsg = cmsg_firsthdr(mh);
    while (cmsg) {
        size_t need = cmsg_len(0);
        if (cmsg->cmsg_len < need || cmsg->cmsg_len > mh->msg_controllen) {
            scm_rights_clear(out);
            return -EINVAL;
        }
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t payload = cmsg->cmsg_len - need;
            if (payload == 0 || (payload % sizeof(int)) != 0 ||
                payload / sizeof(int) > NET_SCM_MAX_FDS) {
                scm_rights_clear(out);
                return -EINVAL;
            }
            int n = (int)(payload / sizeof(int));
            const int *fds = (const int *)cmsg_data_const(cmsg);
            for (int i = 0; i < n; i++) {
                if (out->nfiles >= NET_SCM_MAX_FDS) {
                    scm_rights_clear(out);
                    return -EINVAL;
                }
                int64_t gfd = fdtable_get_current(fds[i]);
                vfile_t *vf = gfd >= 0 ? vfs_get_file_ref((int)gfd) : NULL;
                if (!vf) {
                    scm_rights_clear(out);
                    return -EBADF;
                }
                out->files[out->nfiles++] = vf;
            }
        }
        cmsg = cmsg_nxthdr(mh, cmsg);
    }
    return 0;
}

static size_t build_recv_control(const socket_msghdr_t *mh,
                                 const net_recv_meta_t *meta,
                                 socket_recv_meta_t *out)
{
    size_t used = 0;
    uint8_t *dst = (uint8_t *)mh->msg_control;
    socket_cmsghdr_t *cmsg;

    if (!dst || mh->msg_controllen < sizeof(socket_cmsghdr_t))
        return 0;

    if (out->has_pktinfo) {
        size_t need = cmsg_space(sizeof(socket_in6_pktinfo_t));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = (uint32_t)cmsg_len(sizeof(socket_in6_pktinfo_t));
        socket_in6_pktinfo_t pktinfo = {0};
        if (meta->has_pktinfo) {
            memcpy(pktinfo.ipi6_addr, meta->pktinfo_addr, sizeof(pktinfo.ipi6_addr));
            pktinfo.ipi6_ifindex = meta->pktinfo_ifindex;
        }
        memcpy(cmsg_data(cmsg), &pktinfo, sizeof(pktinfo));
        used += need;
        out->has_pktinfo = 1;
    }

    if (out->has_hoplimit) {
        size_t need = cmsg_space(sizeof(int));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_HOPLIMIT;
        cmsg->cmsg_len = (uint32_t)cmsg_len(sizeof(int));
        int hoplimit = meta->has_hoplimit ? meta->hoplimit : 0;
        memcpy(cmsg_data(cmsg), &hoplimit, sizeof(hoplimit));
        used += need;
        out->has_hoplimit = 1;
    }

    if (out->has_tclass) {
        size_t need = cmsg_space(sizeof(int));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_TCLASS;
        cmsg->cmsg_len = (uint32_t)cmsg_len(sizeof(int));
        int tclass = meta->has_tclass ? meta->tclass : 0;
        memcpy(cmsg_data(cmsg), &tclass, sizeof(tclass));
        used += need;
        out->has_tclass = 1;
    }

    if (out->has_2292_pktinfo) {
        size_t need = cmsg_space(sizeof(socket_in6_pktinfo_t));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_2292PKTINFO;
        cmsg->cmsg_len = (uint32_t)cmsg_len(sizeof(socket_in6_pktinfo_t));
        socket_in6_pktinfo_t pktinfo = {0};
        if (meta->has_pktinfo) {
            memcpy(pktinfo.ipi6_addr, meta->pktinfo_addr, sizeof(pktinfo.ipi6_addr));
            pktinfo.ipi6_ifindex = meta->pktinfo_ifindex;
        }
        memcpy(cmsg_data(cmsg), &pktinfo, sizeof(pktinfo));
        used += need;
        out->has_2292_pktinfo = 1;
    }

    if (out->has_2292_hoplimit) {
        size_t need = cmsg_space(sizeof(int));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_2292HOPLIMIT;
        cmsg->cmsg_len = (uint32_t)cmsg_len(sizeof(int));
        int hoplimit = meta->has_hoplimit ? meta->hoplimit : 0;
        memcpy(cmsg_data(cmsg), &hoplimit, sizeof(hoplimit));
        used += need;
        out->has_2292_hoplimit = 1;
    }

    return used;
}

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

    net_socket_t *sock = net_socket_from_file((int)gfd);
    int old_ttl = 0;
    int old_tclass = 0;
    int have_override = 0;
    socket_msghdr_t cmh = *mh;
    void *cbuf = NULL;
    scm_rights_t scm = {0};
    if (mh->msg_control && mh->msg_controllen) {
        cbuf = kmalloc(mh->msg_controllen);
        if (!cbuf) {
            return -ENOMEM;
        }
        if (copy_from_user(cbuf, mh->msg_control, mh->msg_controllen) < 0) {
            kfree(cbuf);
            return -EFAULT;
        }
        cmh.msg_control = cbuf;
        int ttl = 0;
        int tclass = 0;
        int pr = parse_send_control(&cmh, &ttl, &tclass);
        if (pr < 0) {
            kfree(cbuf);
            return pr;
        }
        pr = parse_send_rights(&cmh, &scm);
        if (pr < 0) {
            kfree(cbuf);
            return pr;
        }
        if (sock && sock->domain == AF_INET6 && sock->raw) {
            old_ttl = sock->raw->ttl;
            old_tclass = sock->raw->tos;
            sock->raw->ttl = ttl ? ttl : sock->raw->ttl;
            sock->raw->tos = tclass ? tclass : sock->raw->tos;
            have_override = 1;
        }
    }

    uint8_t kaddr[NET_SOCKADDR_MAX];
    const void *ka = NULL;
    if (mh->msg_name) {
        if (mh->msg_namelen == 0 || mh->msg_namelen > NET_SOCKADDR_MAX)
            goto out_send;
        if (copy_from_user(kaddr, mh->msg_name, mh->msg_namelen) < 0)
            goto out_send;
        ka = kaddr;
    }
    if (scm.nfiles > 0) {
        /* SCM_RIGHTS is only defined for AF_UNIX sockets. */
        if (!sock || sock->domain != AF_UNIX) {
            scm_rights_clear(&scm);
            if (have_override && sock && sock->raw) {
                sock->raw->ttl = old_ttl;
                sock->raw->tos = old_tclass;
            }
            kfree(cbuf);
            return -EOPNOTSUPP;
        }
        int64_t r = net_unix_socket_sendto_fds(sock, buf, total, ka,
                                               mh->msg_namelen,
                                               scm.files, scm.nfiles);
        if (r < 0)
            scm_rights_clear(&scm);
        if (have_override && sock && sock->raw) {
            sock->raw->ttl = old_ttl;
            sock->raw->tos = old_tclass;
        }
        kfree(cbuf);
        return r;
    }
    {
        int64_t r = net_sendto((int)gfd, buf, total, flags, ka, mh->msg_namelen);
        if (have_override && sock && sock->raw) {
            sock->raw->ttl = old_ttl;
            sock->raw->tos = old_tclass;
        }
        kfree(cbuf);
        return r;
    }

out_send:
    if (scm.nfiles > 0)
        scm_rights_clear(&scm);
    if (have_override && sock && sock->raw) {
        sock->raw->ttl = old_ttl;
        sock->raw->tos = old_tclass;
    }
    kfree(cbuf);
    return -EINVAL;
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

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) {
        kfree(iov);
        return gfd;
    }
    net_socket_t *sock = net_socket_from_file((int)gfd);

    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = 0;
    uint32_t namelen = 0;
    void *addr = NULL;
    size_t *addrlen = NULL;
    if (mh.msg_name) {
        if (copy_from_user(&namelen, &((socket_msghdr_t *)msg)->msg_namelen,
                           sizeof(namelen)) < 0) {
            kfree(iov);
            return -EFAULT;
        }
        klen = namelen;
        if (klen > NET_SOCKADDR_MAX) {
            kfree(iov);
            return -EINVAL;
        }
        addr = kaddr;
        addrlen = &klen;
    }

    net_recv_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    int64_t r = net_recvfrom_meta((int)gfd, buf, total, flags, addr, addrlen, &meta);
    if (r >= 0) {
        size_t copied = 0;
        for (int i = 0; i < mh.msg_iovlen && copied < (size_t)r; i++) {
            size_t n = (size_t)r - copied;
            if (n > iov[i].len)
                n = iov[i].len;
            if (n && (!iov[i].base ||
                      copy_to_user(iov[i].base, buf + copied, n) < 0)) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return -EFAULT;
            }
            copied += n;
        }
        if (mh.msg_name) {
            if (copy_to_user(mh.msg_name, kaddr, klen) < 0) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return -EFAULT;
            }
            namelen = (uint32_t)klen;
            if (copy_to_user(&((socket_msghdr_t *)msg)->msg_namelen,
                             &namelen, sizeof(namelen)) < 0) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return -EFAULT;
            }
        }
        int ctrunc = 0;
        if (mh.msg_control && mh.msg_controllen > 0) {
            socket_recv_meta_t recv_meta = {0};
            size_t used = 0;
            int want_control = meta.scm_nfiles > 0;
            if (sock && sock->domain == AF_INET6 &&
                (sock->ipv6_recv_pktinfo || sock->ipv6_recv_hoplimit ||
                 sock->ipv6_recv_tclass || sock->ipv6_recv_2292_pktinfo ||
                 sock->ipv6_recv_2292_hoplimit)) {
                want_control = 1;
                if (sock->ipv6_recv_pktinfo) {
                    recv_meta.has_pktinfo = 1;
                    if (meta.has_pktinfo) {
                        recv_meta.pktinfo_ifindex = meta.pktinfo_ifindex;
                        memcpy(recv_meta.pktinfo_addr, meta.pktinfo_addr, sizeof(recv_meta.pktinfo_addr));
                    }
                }
                if (sock->ipv6_recv_hoplimit) {
                    recv_meta.has_hoplimit = 1;
                    recv_meta.hoplimit = meta.has_hoplimit ? meta.hoplimit : 0;
                }
                if (sock->ipv6_recv_tclass) {
                    recv_meta.has_tclass = 1;
                    recv_meta.tclass = meta.has_tclass ? meta.tclass : 0;
                }
                if (sock->ipv6_recv_2292_pktinfo) {
                    recv_meta.has_2292_pktinfo = 1;
                    if (meta.has_pktinfo) {
                        recv_meta.pktinfo_ifindex = meta.pktinfo_ifindex;
                        memcpy(recv_meta.pktinfo_addr, meta.pktinfo_addr, sizeof(recv_meta.pktinfo_addr));
                    }
                }
                if (sock->ipv6_recv_2292_hoplimit) {
                    recv_meta.has_2292_hoplimit = 1;
                    recv_meta.hoplimit = meta.has_hoplimit ? meta.hoplimit : 0;
                }
            }
            uint8_t *cbuf = NULL;
            if (want_control) {
                cbuf = (uint8_t *)kmalloc(mh.msg_controllen);
                if (!cbuf) {
                    net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                    kfree(iov);
                    return -ENOMEM;
                }
                if (recv_meta.has_pktinfo || recv_meta.has_hoplimit ||
                    recv_meta.has_tclass || recv_meta.has_2292_pktinfo ||
                    recv_meta.has_2292_hoplimit) {
                    socket_msghdr_t cmh = mh;
                    cmh.msg_control = cbuf;
                    cmh.msg_controllen = mh.msg_controllen;
                    used = build_recv_control(&cmh, &meta, &recv_meta);
                }
            }
            /*
             * SCM_RIGHTS delivery: install the received vfile refs into
             * the receiving process fd table and report the new fds in an
             * SCM_RIGHTS cmsg.  Fds that cannot be reported (no control
             * buffer, truncation, EMFILE) are closed, per Linux.
             */
            if (meta.scm_nfiles > 0) {
                size_t need = cmsg_space(meta.scm_nfiles * sizeof(int));
                if (!cbuf || used + need > mh.msg_controllen) {
                    ctrunc = 1;
                } else {
                    socket_cmsghdr_t *cmsg = (socket_cmsghdr_t *)(cbuf + used);
                    int *fdout = (int *)cmsg_data(cmsg);
                    int delivered = 0;
                    for (int i = 0; i < meta.scm_nfiles; i++) {
                        vfile_t *vf = meta.scm_files[i];
                        meta.scm_files[i] = NULL;
                        if (!vf)
                            continue;
                        int g2 = vfs_alloc_fd(vf);
                        if (g2 < 0) {
                            vfile_put_ref_only(vf);
                            ctrunc = 1;
                            continue;
                        }
                        int u2 = fdtable_install_current(
                            g2, (flags & MSG_CMSG_CLOEXEC) ? O_CLOEXEC : 0);
                        if (u2 < 0) {
                            vfs_close(g2);
                            ctrunc = 1;
                            continue;
                        }
                        fdout[delivered++] = u2;
                    }
                    if (delivered > 0) {
                        cmsg->cmsg_level = SOL_SOCKET;
                        cmsg->cmsg_type = SCM_RIGHTS;
                        cmsg->cmsg_len =
                            (uint32_t)cmsg_len((size_t)delivered * sizeof(int));
                        used += cmsg_space((size_t)delivered * sizeof(int));
                    } else {
                        ctrunc = 1;
                    }
                }
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
            }
            if (cbuf) {
                if (used > 0 && copy_to_user(mh.msg_control, cbuf, used) < 0) {
                    kfree(cbuf);
                    kfree(iov);
                    return -EFAULT;
                }
                kfree(cbuf);
            }
            mh.msg_controllen = (uint32_t)used;
            if (copy_to_user(&((socket_msghdr_t *)msg)->msg_controllen,
                             &mh.msg_controllen, sizeof(mh.msg_controllen)) < 0) {
                kfree(iov);
                return -EFAULT;
            }
        } else if (meta.scm_nfiles > 0) {
            /* No control buffer at all: received fds are closed. */
            net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
            ctrunc = 1;
        }
        mh.msg_flags = ctrunc ? MSG_CTRUNC : 0;
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
