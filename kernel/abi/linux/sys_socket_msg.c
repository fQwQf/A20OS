#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "net/socket/socket_internal.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "ipc/envelope.h"
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
#include "ipc/envelope_acquire_race_test.h"
#endif
#include "abi/linux/scm_rights.h"
#ifdef CONFIG_LINUX_MMSG_TXN_RUST
#include "abi/linux/mmsg_transaction.h"
#endif

typedef struct {
    void *msg_name;
    uint32_t msg_namelen;
    void *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
} socket_msghdr_t;

typedef struct {
    socket_msghdr_t msg_hdr;
    unsigned msg_len;
} socket_mmsghdr_t;

typedef struct { int32_t tv_sec, tv_nsec; } socket_timespec32_t;
typedef struct { int64_t tv_sec, tv_nsec; } socket_timespec64_t;

STATIC_ASSERT(sizeof(socket_timespec32_t) == 8,
              linux_time32_timespec_wire_size);
STATIC_ASSERT(offsetof(socket_timespec32_t, tv_nsec) == 4,
              linux_time32_timespec_nsec_offset);
STATIC_ASSERT(sizeof(socket_timespec64_t) == 16,
              linux_time64_timespec_wire_size);
STATIC_ASSERT(offsetof(socket_timespec64_t, tv_nsec) == 8,
              linux_time64_timespec_nsec_offset);

typedef struct {
    void *base;
    size_t len;
} socket_iovec_t;

/* The Rust adapter receives a socket_msghdr_t in `[usize; 8]`; keep its
 * capacity and alignment valid on every supported 32- and 64-bit target. */
STATIC_ASSERT(sizeof(socket_msghdr_t) <= 8 * sizeof(size_t),
              mmsg_header_fits_rust_scratch);
STATIC_ASSERT(__alignof__(socket_msghdr_t) <= __alignof__(size_t),
              mmsg_header_rust_scratch_alignment);

typedef struct {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
} socket_cmsghdr_t;

typedef struct {
    uint8_t ipi6_addr[16];
    unsigned ipi6_ifindex;
} socket_in6_pktinfo_t;

typedef struct {
    int32_t ifindex;
    uint8_t spec_dst[4];
    uint8_t addr[4];
} socket_in_pktinfo_t;

typedef struct {
    int has_ipv4_pktinfo;
    int has_pktinfo;
    int has_hoplimit;
    int has_tclass;
    int has_2292_pktinfo;
    int has_2292_hoplimit;
    uint32_t pktinfo_ifindex;
    uint8_t pktinfo_addr[16];
    int hoplimit;
    int tclass;
    int has_cred;
    int32_t cred_pid;
    int32_t cred_uid;
    int32_t cred_gid;
} socket_recv_meta_t;

/* Both references belong to one fd-table resolution. Keep the vfile alive
 * until the syscall is done so close/reuse cannot run its socket close hook
 * during a blocking operation or while post-receive metadata is consumed. */
static int64_t linux_socket_unpin_result(net_socket_t *sock, vfile_t *vf,
                                         int64_t result)
{
    if (sock)
        net_socket_free(sock);
    if (vf)
        vfs_put_file(vf);
    return result;
}

/*
 * SCM_RIGHTS fds resolved from a sendmsg control buffer.  Each entry is a
 * vfile reference (dup semantics) owned by this struct until the message
 * is enqueued or the refs are dropped.
 */
#ifndef CONFIG_LINUX_SCM_RIGHTS_RUST
typedef struct {
    int nfiles;
    vfile_t *files[NET_SCM_MAX_FDS];
} scm_rights_t;
#endif

#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
typedef struct {
    net_socket_t *sock;
    const void *buf;
    size_t len;
    const void *addr;
    size_t addrlen;
} linux_scm_send_context_t;

static int linux_scm_acquire_send_fd(int fd, void *ctx, void **out)
{
    (void)ctx;
    if (!out)
        return -EINVAL;
    *out = NULL;
    vfile_t *vf = fdtable_get_current_file_ref(fd);
    if (!vf)
        return -EBADF;
    if (env_active(proc_current())) {
        /* The provider validates that this numeric slot still names the
         * pinned object, then checks the object's vfile-bound class and charges
         * policy at the fdtable lock linearization point. */
        int mr = env_mediate_send_vfile(fd, vf);
        if (mr) {
            vfs_put_file(vf);
            return mr;
        }
    }
    *out = vf;
    return 0;
}

static void linux_scm_release_file(void *ctx, void *file)
{
    (void)ctx;
    vfs_put_file((vfile_t *)file);
}

static int64_t linux_scm_enqueue_send(void *opaque, void **files,
                                      int count)
{
    linux_scm_send_context_t *ctx = (linux_scm_send_context_t *)opaque;
    if (!ctx || !ctx->sock || ctx->sock->domain != AF_UNIX)
        return -EOPNOTSUPP;
    return net_unix_socket_sendto_fds(
        ctx->sock, ctx->buf, ctx->len, ctx->addr, ctx->addrlen,
        (vfile_t **)files, count);
}

static int linux_scm_install_recv_file(void *opaque, void *file, int cloexec,
                                       void **pinned_out)
{
    (void)opaque;
    vfile_t *vf = (vfile_t *)file;
    if (!vf || !pinned_out) {
        if (vf)
            vfs_put_file(vf);
        return -EINVAL;
    }
    *pinned_out = NULL;
    /* Keep a stable identity pin until copyout commits or rollback atomically
     * verifies and closes this precise table entry. */
    vfile_get(vf);
    int gfd = fdtable_install_current_vfile(vf, cloexec ? O_CLOEXEC : 0);
    if (gfd < 0) {
        vfs_put_file(vf);
        vfs_put_file(vf);
        return gfd;
    }
    if (env_active(proc_current())) {
        const uint64_t base_rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                                     A20_RIGHT_STAT | A20_RIGHT_SEEK;
        int mr = 0;
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
        int injected = a20_envelope_race_test_before_mediation(
            A20_ENVELOPE_RACE_OP_SCM, gfd, vf);
        if (injected < 0)
            mr = injected;
#endif
        if (!mr)
            mr = env_mediate_acquire_vfile(gfd, vf, base_rights);
        if (mr) {
            (void)fdtable_close_current_if_file(gfd, vf);
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
            a20_envelope_race_test_note_pin_release(
                A20_ENVELOPE_RACE_OP_SCM, vf->identity);
#endif
            vfs_put_file(vf);
            return mr;
        }
    }
    *pinned_out = vf;
    return gfd;
}

static void linux_scm_close_if_file(void *opaque, int fd, void *expected)
{
    (void)opaque;
    (void)fdtable_close_current_if_file(fd, (vfile_t *)expected);
}
#else
static void linux_scm_c_recv_abort(int *fds, vfile_t **files, int *count)
{
    while (*count > 0) {
        int i = --*count;
        (void)fdtable_close_current_if_file(fds[i], files[i]);
        vfs_put_file(files[i]);
        files[i] = NULL;
        fds[i] = -1;
    }
}

static void linux_scm_c_recv_commit(vfile_t **files, int *count)
{
    while (*count > 0) {
        vfs_put_file(files[--*count]);
        files[*count] = NULL;
    }
}
#endif

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

static size_t cmsg_remaining(const socket_msghdr_t *mh,
                             const socket_cmsghdr_t *cmsg)
{
    size_t offset = (const uint8_t *)cmsg - (const uint8_t *)mh->msg_control;
    return offset <= mh->msg_controllen ? mh->msg_controllen - offset : 0;
}

static socket_cmsghdr_t *cmsg_nxthdr(const socket_msghdr_t *mh,
                                   const socket_cmsghdr_t *cmsg)
{
    size_t remaining = cmsg_remaining(mh, cmsg);
    if (cmsg->cmsg_len < sizeof(*cmsg) || cmsg->cmsg_len > remaining ||
        cmsg->cmsg_len > SIZE_MAX - (sizeof(size_t) - 1))
        return NULL;
    size_t step = cmsg_align(cmsg->cmsg_len);
    if (step > remaining || remaining - step < sizeof(*cmsg))
        return NULL;
    return (socket_cmsghdr_t *)((uint8_t *)cmsg + step);
}

static int parse_send_control(const socket_msghdr_t *mh, int *ttl, int *tclass,
    net_datagram_control_t *control, int *have_control)
{
    if (!mh->msg_control || mh->msg_controllen == 0)
        return 0;
    if (mh->msg_controllen < sizeof(socket_cmsghdr_t))
        return -EINVAL;
    socket_cmsghdr_t *cmsg = cmsg_firsthdr(mh);
    while (cmsg) {
        size_t need = cmsg_len(0);
        if (cmsg->cmsg_len < need || cmsg->cmsg_len > cmsg_remaining(mh, cmsg))
            return -EINVAL;
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == 8 /* IP_PKTINFO */) {
            if (cmsg->cmsg_len != cmsg_len(sizeof(socket_in_pktinfo_t))) return -EINVAL;
            socket_in_pktinfo_t info;
            memcpy(&info, cmsg_data_const(cmsg), sizeof(info));
            if (info.ifindex < 0) return -EINVAL;
            control->ifindex = (uint32_t)info.ifindex;
            memcpy(&control->source_v4, info.spec_dst, 4);
            *have_control = 1;
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT) {
            if (cmsg->cmsg_len < cmsg_len(sizeof(int)))
                return -EINVAL;
            memcpy(ttl, cmsg_data_const(cmsg), sizeof(int));
        } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_TCLASS) {
            if (cmsg->cmsg_len < cmsg_len(sizeof(int)))
                return -EINVAL;
            memcpy(tclass, cmsg_data_const(cmsg), sizeof(int));
        } else if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
            /* SCM_CREDENTIALS on sendmsg is advisory on Linux; the kernel
             * records the sending process credentials regardless.  Accept
             * the cmsg (validate shape) but do not let a caller spoof the
             * recorded pid/uid/gid. */
            if (cmsg->cmsg_len < cmsg_len(3 * sizeof(int32_t)))
                return -EINVAL;
        }
        cmsg = cmsg_nxthdr(mh, cmsg);
    }
    if (*ttl < -1 || *ttl > 255 || *tclass < -1 || *tclass > 255)
        return -EINVAL;
    if (*have_control && (*ttl >= 0 || *tclass >= 0))
        return -EINVAL;
    return 0;
}

#ifndef CONFIG_LINUX_SCM_RIGHTS_RUST
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
    if (!mh->msg_control || mh->msg_controllen == 0)
        return 0;
    if (mh->msg_controllen < sizeof(socket_cmsghdr_t))
        return -EINVAL;
    socket_cmsghdr_t *cmsg = cmsg_firsthdr(mh);
    while (cmsg) {
        size_t need = cmsg_len(0);
        if (cmsg->cmsg_len < need || cmsg->cmsg_len > cmsg_remaining(mh, cmsg)) {
            scm_rights_clear(out);
            return -EINVAL;
        }
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t payload = cmsg->cmsg_len - need;
            /* A header with no descriptors is valid (e.g. request/reply
             * messages whose optional handle list is empty). Keep parsing
             * later headers and send the ordinary payload as usual. */
            if ((payload % sizeof(int)) != 0 ||
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
                vfile_t *vf = fdtable_get_current_file_ref(fds[i]);
                if (!vf) {
                    scm_rights_clear(out);
                    return -EBADF;
                }
                if (env_active(proc_current())) {
                    /* Revalidate (fd, pinned vfile) atomically inside the
                     * identity-bound envelope provider. A close/reuse race
                     * fails closed instead of authorizing the replacement. */
                    int ms = env_mediate_send_vfile(fds[i], vf);
                    if (ms) {
                        vfs_put_file(vf);
                        scm_rights_clear(out);
                        return ms;
                    }
                }
                out->files[out->nfiles++] = vf;
            }
        }
        cmsg = cmsg_nxthdr(mh, cmsg);
    }
    return 0;
}
#endif

static void pktinfo6_address(uint8_t *out, const net_recv_meta_t *meta)
{
    if (meta->pktinfo_family == AF_INET) {
        memset(out, 0, 16);
        out[10] = out[11] = 0xff;
        memcpy(out + 12, meta->pktinfo.ipv4.destination, 4);
    } else {
        memcpy(out, meta->pktinfo_addr, 16);
    }
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

    if (out->has_ipv4_pktinfo) {
        size_t need = cmsg_space(sizeof(socket_in_pktinfo_t));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IP;
        cmsg->cmsg_type = 8; /* IP_PKTINFO */
        cmsg->cmsg_len = cmsg_len(sizeof(socket_in_pktinfo_t));
        socket_in_pktinfo_t info = {0};
        if (meta->has_pktinfo && meta->pktinfo_family == AF_INET) {
            info.ifindex = (int32_t)meta->pktinfo_ifindex;
            memcpy(info.spec_dst, meta->pktinfo.ipv4.local, 4);
            memcpy(info.addr, meta->pktinfo.ipv4.destination, 4);
        }
        memcpy(cmsg_data(cmsg), &info, sizeof(info));
        used += need;
    }

    if (out->has_pktinfo) {
        size_t need = cmsg_space(sizeof(socket_in6_pktinfo_t));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = IPPROTO_IPV6;
        cmsg->cmsg_type = IPV6_PKTINFO;
        cmsg->cmsg_len = cmsg_len(sizeof(socket_in6_pktinfo_t));
        socket_in6_pktinfo_t pktinfo = {0};
        if (meta->has_pktinfo) {
            pktinfo6_address(pktinfo.ipi6_addr, meta);
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
        cmsg->cmsg_len = cmsg_len(sizeof(int));
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
        cmsg->cmsg_len = cmsg_len(sizeof(int));
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
        cmsg->cmsg_len = cmsg_len(sizeof(socket_in6_pktinfo_t));
        socket_in6_pktinfo_t pktinfo = {0};
        if (meta->has_pktinfo) {
            pktinfo6_address(pktinfo.ipi6_addr, meta);
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
        cmsg->cmsg_len = cmsg_len(sizeof(int));
        int hoplimit = meta->has_hoplimit ? meta->hoplimit : 0;
        memcpy(cmsg_data(cmsg), &hoplimit, sizeof(hoplimit));
        used += need;
        out->has_2292_hoplimit = 1;
    }

    if (out->has_cred) {
        /* Linux struct ucred: { pid, uid, gid }. */
        size_t need = cmsg_space(3 * sizeof(int32_t));
        if (used + need > mh->msg_controllen)
            return used;
        cmsg = (socket_cmsghdr_t *)(dst + used);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_CREDENTIALS;
        cmsg->cmsg_len = cmsg_len(3 * sizeof(int32_t));
        int32_t cred[3];
        cred[0] = meta->has_cred ? out->cred_pid : 0;
        cred[1] = meta->has_cred ? out->cred_uid : 0;
        cred[2] = meta->has_cred ? out->cred_gid : 0;
        memcpy(cmsg_data(cmsg), cred, sizeof(cred));
        used += need;
    }

    return used;
}

static int socket_timespec_read(const void *timeout, int time64,
                                int64_t *sec, int64_t *nsec)
{
    int64_t s, ns;
    if (time64) {
        socket_timespec64_t ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        s = ts.tv_sec;
        ns = ts.tv_nsec;
    } else {
        socket_timespec32_t ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        s = ts.tv_sec;
        ns = ts.tv_nsec;
    }
    if (s < 0 || ns < 0 || ns >= 1000000000LL)
        return -EINVAL;
    *sec = s;
    *nsec = ns;
    return 0;
}

#ifndef CONFIG_LINUX_MMSG_TXN_RUST
static int socket_timespec_to_deadline(const void *timeout, int time64,
                                       uint64_t *deadline)
{
    int64_t sec, nsec;
    int rc = socket_timespec_read(timeout, time64, &sec, &nsec);
    if (rc < 0)
        return rc;
    uint64_t tps = TICKS_PER_SEC;
    uint64_t whole = (uint64_t)sec * tps;
    uint64_t part = (uint64_t)nsec * tps / 1000000000ULL;
    if (sec && whole / tps != (uint64_t)sec)
        return -EINVAL;
    uint64_t ticks = whole + part;
    if (ticks < whole)
        return -EINVAL;
    if (ticks == 0 && (sec || nsec))
        ticks = 1;
    uint64_t now = timer_get_ticks();
    *deadline = ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
    return 0;
}

static int socket_timespec_write_remaining(void *timeout, uint64_t deadline,
                                           int time64)
{
    uint64_t tps = TICKS_PER_SEC;
    uint64_t now = timer_get_ticks();
    uint64_t remaining = deadline > now ? deadline - now : 0;
    uint64_t sec = remaining / tps;
    uint64_t nsec = ((remaining % tps) * 1000000000ULL) / tps;
    int64_t out_sec = sec > INT64_MAX ? INT64_MAX : (int64_t)sec;
    if (time64) {
        socket_timespec64_t ts = { out_sec, (int64_t)nsec };
        return copy_to_user(timeout, &ts, sizeof(ts)) < 0 ? -EFAULT : 0;
    }
    socket_timespec32_t ts = {
        out_sec > INT32_MAX ? INT32_MAX : (int32_t)out_sec,
        (int32_t)nsec,
    };
    return copy_to_user(timeout, &ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}
#endif

#ifdef CONFIG_LINUX_MMSG_TXN_RUST
static int socket_timespec_write(void *timeout, int64_t sec, int64_t nsec,
                                 int time64)
{
    if (time64) {
        socket_timespec64_t ts = { sec, nsec };
        return copy_to_user(timeout, &ts, sizeof(ts)) < 0 ? -EFAULT : 0;
    }
    socket_timespec32_t ts = {
        sec > INT32_MAX ? INT32_MAX : sec < INT32_MIN ? INT32_MIN : (int32_t)sec,
        nsec > INT32_MAX ? INT32_MAX : nsec < INT32_MIN ? INT32_MIN : (int32_t)nsec,
    };
    return copy_to_user(timeout, &ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}
#endif

#ifdef CONFIG_LINUX_MMSG_TXN_RUST
extern int64_t a20_linux_sendmmsg_transaction(
    void *ctx, void *user_mmsg, unsigned vlen, int flags,
    size_t header_size, const a20_linux_mmsg_ops_t *ops);
extern int64_t a20_linux_recvmmsg_transaction(
    void *ctx, void *user_mmsg, unsigned vlen, int flags,
    void *user_timeout, size_t header_size,
    const a20_linux_mmsg_ops_t *ops);

static int64_t linux_sendmsg_with_socket(int fd, const socket_msghdr_t *mh,
                                         int flags,
                                         net_socket_t *pinned_sock,
                                         vfile_t *pinned_vf);
static int64_t linux_recvmsg_with_socket_header(
    int fd, void *msg, const socket_msghdr_t *kernel_header, int flags,
    net_socket_t *pinned_sock, vfile_t *pinned_vf, uint64_t deadline);

typedef struct linux_mmsg_adapter {
    int fd;
    net_socket_t *sock;
    vfile_t *sock_vf;
    void *current_user_header;
    int timeout_time64;
} linux_mmsg_adapter_t;

static int linux_mmsg_copy_header(void *ctx, void *user_mmsg,
                                  unsigned index, void *header_out,
                                  size_t header_size)
{
    if (!user_mmsg || !header_out || header_size != sizeof(socket_msghdr_t))
        return -EFAULT;
    uintptr_t base = (uintptr_t)user_mmsg;
    size_t offset = (size_t)index * sizeof(socket_mmsghdr_t);
    if (offset > UINTPTR_MAX - base)
        return -EFAULT;
    void *src = (void *)(base + offset);
    if (copy_from_user(header_out, src, header_size) < 0)
        return -EFAULT;
    ((linux_mmsg_adapter_t *)ctx)->current_user_header = src;
    return 0;
}

static int linux_mmsg_copy_len(void *ctx, void *user_mmsg,
                               unsigned index, uint32_t msg_len)
{
    (void)ctx;
    if (!user_mmsg)
        return -EFAULT;
    uintptr_t base = (uintptr_t)user_mmsg;
    size_t offset = (size_t)index * sizeof(socket_mmsghdr_t) +
                    offsetof(socket_mmsghdr_t, msg_len);
    if (offset > UINTPTR_MAX - base)
        return -EFAULT;
    return copy_to_user((void *)(base + offset), &msg_len, sizeof(msg_len)) < 0
        ? -EFAULT : 0;
}

static int64_t linux_mmsg_send_one(void *opaque, void *header, int flags)
{
    linux_mmsg_adapter_t *ctx = opaque;
    return linux_sendmsg_with_socket(ctx->fd, header, flags,
                                     ctx->sock, ctx->sock_vf);
}

static int64_t linux_mmsg_recv_one(void *opaque, void *header, int flags,
                                  uint64_t deadline)
{
    linux_mmsg_adapter_t *ctx = opaque;
    return linux_recvmsg_with_socket_header(
        ctx->fd, ctx->current_user_header, header, flags,
        ctx->sock, ctx->sock_vf, deadline);
}

static int linux_mmsg_read_timeout(void *ctx, const void *user_timeout,
                                   int64_t *sec, int64_t *nsec)
{
    return socket_timespec_read(user_timeout,
        ((linux_mmsg_adapter_t *)ctx)->timeout_time64, sec, nsec);
}

static int linux_mmsg_write_timeout(void *ctx, void *user_timeout,
                                    int64_t sec, int64_t nsec)
{
    return socket_timespec_write(user_timeout, sec, nsec,
        ((linux_mmsg_adapter_t *)ctx)->timeout_time64);
}

static uint64_t linux_mmsg_now_ticks(void *ctx)
{
    (void)ctx;
    return timer_get_ticks();
}

static uint64_t linux_mmsg_ticks_per_sec(void *ctx)
{
    (void)ctx;
    return TICKS_PER_SEC;
}

static const a20_linux_mmsg_ops_t linux_mmsg_ops = {
    .copy_header = linux_mmsg_copy_header,
    .copy_msg_len = linux_mmsg_copy_len,
    .send_one = linux_mmsg_send_one,
    .recv_one = linux_mmsg_recv_one,
    .read_timeout = linux_mmsg_read_timeout,
    .write_timeout = linux_mmsg_write_timeout,
    .now_ticks = linux_mmsg_now_ticks,
    .ticks_per_sec = linux_mmsg_ticks_per_sec,
};
#endif

static int64_t linux_sendmsg_with_socket(int fd, const socket_msghdr_t *mh,
                                         int flags,
                                         net_socket_t *pinned_sock,
                                         vfile_t *pinned_vf)
{
    if (!mh) return -EFAULT;
    vfile_t *sock_vf = pinned_vf;
    net_socket_t *sock = pinned_sock;
    if (sock && sock_vf) {
        net_socket_ref(sock);
        vfile_get(sock_vf);
    } else {
        sock_vf = NULL;
        sock = NULL;
        int pin = net_socket_pin_current_fd(fd, &sock_vf, &sock);
        if (pin < 0)
            return pin;
    }
    if (mh->msg_iovlen > 1024)
        return linux_socket_unpin_result(sock, sock_vf, -EMSGSIZE);

    size_t iov_bytes = (size_t)mh->msg_iovlen * sizeof(socket_iovec_t);
    socket_iovec_t *iov = iov_bytes ? (socket_iovec_t *)kmalloc(iov_bytes) : NULL;
    if (iov_bytes && !iov)
        return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
    if (iov_bytes && copy_from_user(iov, mh->msg_iov, iov_bytes) < 0) {
        kfree(iov);
        return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
    }

    size_t total = 0;
    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len > SIZE_MAX - total) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EINVAL);
        }
        total += iov[i].len;
    }
    /* One data-plane charge for the complete send, including empty UDP
     * datagrams. Zero payload still carries ancillary and destination state. */
    if (env_active(proc_current())) {
        int64_t sgfd = fdtable_get_current(fd);
        if (sgfd < 0) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, sgfd);
        }
        int mr = env_mediate_use_dir((int)sgfd, total, 1);
        if (mr) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, mr);
        }
    }

    uint8_t *buf = total ? (uint8_t *)proc_scratch_buffer(total) : (uint8_t *)"";
    if (!buf) {
        kfree(iov);
        return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
    }
    size_t off = 0;
    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (iov[i].len == 0)
            continue;
        if (!iov[i].base) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
        }
        if (copy_from_user(buf + off, iov[i].base, iov[i].len) < 0) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
        }
        off += iov[i].len;
    }
    kfree(iov);

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return linux_socket_unpin_result(sock, sock_vf, gfd);
    int packet_hop_limit = -1;
    int packet_tclass = -1;
    int have_ipv4_control = 0;
    net_datagram_control_t ipv4_control = {0};
    socket_msghdr_t cmh = *mh;
    void *cbuf = NULL;
#ifndef CONFIG_LINUX_SCM_RIGHTS_RUST
    scm_rights_t scm = {0};
#endif
    if (mh->msg_control && mh->msg_controllen) {
        cbuf = kmalloc(mh->msg_controllen);
        if (!cbuf) {
            return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
        }
        if (copy_from_user(cbuf, mh->msg_control, mh->msg_controllen) < 0) {
            kfree(cbuf);
            return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
        }
        cmh.msg_control = cbuf;
        int pr = parse_send_control(&cmh, &packet_hop_limit, &packet_tclass, &ipv4_control, &have_ipv4_control);
        if (pr < 0) {
            kfree(cbuf);
            return linux_socket_unpin_result(sock, sock_vf, pr);
        }
#ifndef CONFIG_LINUX_SCM_RIGHTS_RUST
        pr = parse_send_rights(&cmh, &scm);
        if (pr < 0) {
            kfree(cbuf);
            return linux_socket_unpin_result(sock, sock_vf, pr);
        }
#endif

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
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
    {
        linux_scm_send_context_t ctx = {
            .sock = sock, .buf = buf, .len = total, .addr = ka,
            .addrlen = mh->msg_namelen,
        };
        int64_t r = a20_linux_scm_rights_send(
            (const uint8_t *)cmh.msg_control, cmh.msg_controllen, &ctx,
            linux_scm_acquire_send_fd, linux_scm_release_file,
            linux_scm_enqueue_send);
        if (r != LINUX_SCM_RIGHTS_NO_RIGHTS) {
            kfree(cbuf);
            return linux_socket_unpin_result(sock, sock_vf, r);
        }
    }
#else
    if (scm.nfiles > 0) {
        /* SCM_RIGHTS is only defined for AF_UNIX sockets. */
        if (!sock || sock->domain != AF_UNIX) {
            scm_rights_clear(&scm);
            kfree(cbuf);
            return linux_socket_unpin_result(sock, sock_vf, -EOPNOTSUPP);
        }
        int64_t r = net_unix_socket_sendto_fds(sock, buf, total, ka,
                                               mh->msg_namelen,
                                               scm.files, scm.nfiles);
        if (r < 0)
            scm_rights_clear(&scm);
        kfree(cbuf);
        return linux_socket_unpin_result(sock, sock_vf, r);
    }
#endif
    {
        int64_t r;
        if (packet_hop_limit >= 0 || packet_tclass >= 0)
            r = net_inet_send_raw_packet(sock, buf, total, ka, mh->msg_namelen,
                                         packet_hop_limit, packet_tclass);
        else
            r = net_sendto_control(sock, buf, total, flags, ka, mh->msg_namelen,
                                     have_ipv4_control ? &ipv4_control : NULL);
        kfree(cbuf);
        return linux_socket_unpin_result(sock, sock_vf, r);
    }

out_send:
#ifndef CONFIG_LINUX_SCM_RIGHTS_RUST
    if (scm.nfiles > 0)
        scm_rights_clear(&scm);
#endif
    kfree(cbuf);
    return linux_socket_unpin_result(sock, sock_vf, -EINVAL);
}

int64_t sys_sendmsg(int fd, const void *msg, int flags)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (copy_from_user(&mh, msg, sizeof(mh)) < 0) return -EFAULT;
    return linux_sendmsg_with_socket(fd, &mh, flags, NULL, NULL);
}

static int64_t linux_recvmsg_with_socket_header(
    int fd, void *msg, const socket_msghdr_t *kernel_header, int flags,
    net_socket_t *pinned_sock, vfile_t *pinned_vf, uint64_t deadline)
{
    if (!msg) return -EFAULT;
    socket_msghdr_t mh;
    if (kernel_header)
        mh = *kernel_header;
    else if (copy_from_user(&mh, msg, sizeof(mh)) < 0)
        return -EFAULT;
    if (mh.msg_iovlen == 0) return 0;
    if (mh.msg_iovlen > 1024) return -EMSGSIZE;

    vfile_t *sock_vf = pinned_vf;
    net_socket_t *sock = pinned_sock;
    if (sock && sock_vf) {
        net_socket_ref(sock);
        vfile_get(sock_vf);
    } else {
        sock_vf = NULL;
        sock = NULL;
        int pin = net_socket_pin_current_fd(fd, &sock_vf, &sock);
        if (pin < 0)
            return pin;
    }

    size_t iov_bytes = (size_t)mh.msg_iovlen * sizeof(socket_iovec_t);
    socket_iovec_t *iov = (socket_iovec_t *)kmalloc(iov_bytes);
    if (!iov)
        return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
    if (copy_from_user(iov, mh.msg_iov, iov_bytes) < 0) {
        kfree(iov);
        return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
    }

    size_t total = 0;
    for (size_t i = 0; i < mh.msg_iovlen; i++) {
        if (iov[i].len > SIZE_MAX - total) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EINVAL);
        }
        total += iov[i].len;
    }
    uint8_t *buf = (uint8_t *)proc_scratch_buffer(total ? total : 1);
    if (!buf) {
        kfree(iov);
        return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
    }

    if (env_active(proc_current())) {
        int64_t gfd = fdtable_get_current(fd);
        if (gfd < 0) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, gfd);
        }
        int mr = env_mediate_use_dir((int)gfd, total, 0);
        if (mr) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, mr);
        }
    }
    uint8_t kaddr[NET_SOCKADDR_MAX];
    size_t klen = 0;
    uint32_t namelen = 0;
    void *addr = NULL;
    size_t *addrlen = NULL;
    if (mh.msg_name) {
        if (copy_from_user(&namelen, &((socket_msghdr_t *)msg)->msg_namelen,
                           sizeof(namelen)) < 0) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
        }
        klen = namelen;
        if (klen > NET_SOCKADDR_MAX) {
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EINVAL);
        }
        addr = kaddr;
        addrlen = &klen;
    }

    /* Pin the socket object through the actual receive. A numeric descriptor
     * lookup followed by a later second lookup could otherwise receive from a
     * replacement socket after concurrent close/reuse. Snapshot ancillary
     * options while the vfile and socket are both pinned; later copyout needs
     * only these values. */
    uint64_t sock_lock_flags = net_sock_lock(sock);
    int sock_domain = sock->domain;
    int sock_passcred = sock->passcred;
    int sock_recv_pktinfo = sock->ipv6_recv_pktinfo;
    int sock_recv_ipv4_pktinfo = sock->ipv4_recv_pktinfo;
    int sock_recv_hoplimit = sock->ipv6_recv_hoplimit;
    int sock_recv_tclass = sock->ipv6_recv_tclass;
    int sock_recv_2292_pktinfo = sock->ipv6_recv_2292_pktinfo;
    int sock_recv_2292_hoplimit = sock->ipv6_recv_2292_hoplimit;
    net_sock_unlock(sock, sock_lock_flags);

    net_recv_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    int64_t r = net_recvfrom_socket_meta_until(sock, buf, total, flags,
                                               addr, addrlen, &meta,
                                               deadline);
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
    linux_scm_recv_txn_t rights_txn = {0};
#else
    int c_rights_fds[NET_SCM_MAX_FDS];
    vfile_t *c_rights_files[NET_SCM_MAX_FDS] = {0};
    int c_rights_count = 0;
#endif
    if (r >= 0) {
        size_t copied = 0;
        for (size_t i = 0; i < mh.msg_iovlen && copied < (size_t)r; i++) {
            size_t n = (size_t)r - copied;
            if (n > iov[i].len)
                n = iov[i].len;
            if (n && (!iov[i].base ||
                      copy_to_user(iov[i].base, buf + copied, n) < 0)) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
            }
            copied += n;
        }
        if (mh.msg_name) {
            if (copy_to_user(mh.msg_name, kaddr, klen) < 0) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
            }
            namelen = (uint32_t)klen;
            if (copy_to_user(&((socket_msghdr_t *)msg)->msg_namelen,
                             &namelen, sizeof(namelen)) < 0) {
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                kfree(iov);
                return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
            }
        }
        int ctrunc = 0;
        if (mh.msg_control && mh.msg_controllen > 0) {
            socket_recv_meta_t recv_meta = {0};
            size_t used = 0;
            int want_control = meta.scm_nfiles > 0;
            if (sock_passcred && meta.has_cred)
                want_control = 1;
            if (sock_domain == AF_INET && sock_recv_ipv4_pktinfo) {
                recv_meta.has_ipv4_pktinfo = 1;
                want_control = 1;
            }
            if (sock_domain == AF_INET6 &&
                (sock_recv_pktinfo || sock_recv_hoplimit ||
                 sock_recv_tclass || sock_recv_2292_pktinfo ||
                 sock_recv_2292_hoplimit)) {
                want_control = 1;
                if (sock_recv_pktinfo) {
                    recv_meta.has_pktinfo = 1;
                    if (meta.has_pktinfo) {
                        recv_meta.pktinfo_ifindex = meta.pktinfo_ifindex;
                        memcpy(recv_meta.pktinfo_addr, meta.pktinfo_addr, sizeof(recv_meta.pktinfo_addr));
                    }
                }
                if (sock_recv_hoplimit) {
                    recv_meta.has_hoplimit = 1;
                    recv_meta.hoplimit = meta.has_hoplimit ? meta.hoplimit : 0;
                }
                if (sock_recv_tclass) {
                    recv_meta.has_tclass = 1;
                    recv_meta.tclass = meta.has_tclass ? meta.tclass : 0;
                }
                if (sock_recv_2292_pktinfo) {
                    recv_meta.has_2292_pktinfo = 1;
                    if (meta.has_pktinfo) {
                        recv_meta.pktinfo_ifindex = meta.pktinfo_ifindex;
                        memcpy(recv_meta.pktinfo_addr, meta.pktinfo_addr, sizeof(recv_meta.pktinfo_addr));
                    }
                }
                if (sock_recv_2292_hoplimit) {
                    recv_meta.has_2292_hoplimit = 1;
                    recv_meta.hoplimit = meta.has_hoplimit ? meta.hoplimit : 0;
                }
            }
            if (sock_passcred && meta.has_cred) {
                recv_meta.has_cred = 1;
                recv_meta.cred_pid = meta.cred_pid;
                recv_meta.cred_uid = meta.cred_uid;
                recv_meta.cred_gid = meta.cred_gid;
            }
            uint8_t *cbuf = NULL;
            if (want_control) {
                cbuf = (uint8_t *)kmalloc(mh.msg_controllen);
                if (!cbuf) {
                    net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
                    kfree(iov);
                    return linux_socket_unpin_result(sock, sock_vf, -ENOMEM);
                }
                /* Ancillary headers use native size_t on the Linux wire.
                 * Clear alignment padding as well as initializing every field. */
                memset(cbuf, 0, mh.msg_controllen);
                if (recv_meta.has_ipv4_pktinfo || recv_meta.has_pktinfo || recv_meta.has_hoplimit ||
                    recv_meta.has_tclass || recv_meta.has_2292_pktinfo ||
                    recv_meta.has_2292_hoplimit || recv_meta.has_cred) {
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
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
                int fdout[LINUX_SCM_RIGHTS_MAX_FDS];
                size_t delivered = 0;
                int rr = a20_linux_scm_rights_recv_begin(
                    (void **)meta.scm_files, meta.scm_nfiles,
                    cbuf ? mh.msg_controllen : 0, used,
                    (flags & MSG_CMSG_CLOEXEC) != 0, NULL,
                    linux_scm_install_recv_file, linux_scm_release_file,
                    &rights_txn, fdout, &delivered);
                meta.scm_nfiles = 0;
                if (rr < 0) {
                    if (cbuf)
                        kfree(cbuf);
                    a20_linux_scm_rights_recv_abort(
                        &rights_txn, NULL, linux_scm_close_if_file,
                        linux_scm_release_file);
                    kfree(iov);
                    return linux_socket_unpin_result(sock, sock_vf, rr);
                }
                if (rr & MSG_CTRUNC)
                    ctrunc = 1;
                if (delivered > 0) {
                    socket_cmsghdr_t *cmsg =
                        (socket_cmsghdr_t *)(cbuf + used);
                    cmsg->cmsg_level = SOL_SOCKET;
                    cmsg->cmsg_type = SCM_RIGHTS;
                    cmsg->cmsg_len = cmsg_len(delivered * sizeof(int));
                    memcpy(cmsg_data(cmsg), fdout, delivered * sizeof(int));
                    size_t remaining = mh.msg_controllen - used;
                    size_t extent = cmsg_space(delivered * sizeof(int));
                    used += extent < remaining ? extent : remaining;
                }
#else
                int fit = meta.scm_nfiles;
                if (!cbuf || used > mh.msg_controllen) {
                    fit = 0;
                } else {
                    while (fit > 0 &&
                           cmsg_len((size_t)fit * sizeof(int)) >
                               mh.msg_controllen - used)
                        fit--;
                }
                if (fit == 0) {
                    ctrunc = 1;
                } else {
                    socket_cmsghdr_t *cmsg = (socket_cmsghdr_t *)(cbuf + used);
                    int *fdout = (int *)cmsg_data(cmsg);
                    int delivered = 0;
                    for (int i = 0; i < fit; i++) {
                        vfile_t *vf = meta.scm_files[i];
                        meta.scm_files[i] = NULL;
                        if (!vf)
                            continue;
                        vfile_get(vf);
                        int g2 = fdtable_install_current_vfile(
                            vf, (flags & MSG_CMSG_CLOEXEC) ? O_CLOEXEC : 0);
                        if (g2 < 0) {
                            vfs_put_file(vf);
                            vfs_put_file(vf);
                            ctrunc = 1;
                            continue;
                        }
                        /* A6 receive side: an incoming descriptor only
                         * installs if the receiver's envelope authorizes
                         * its class (docs/research/05 §2.5.3); denied fds
                         * are closed like other undeliverable ones. */
                        if (env_active(proc_current())) {
                            const uint64_t base_rights = A20_RIGHT_READ |
                                A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                A20_RIGHT_SEEK;
                            int mr = 0;
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
                            int injected =
                                a20_envelope_race_test_before_mediation(
                                    A20_ENVELOPE_RACE_OP_SCM, g2, vf);
                            if (injected < 0)
                                mr = injected;
#endif
                            if (!mr)
                                mr = env_mediate_acquire_vfile(
                                    g2, vf, base_rights);
                            if (mr) {
                                (void)fdtable_close_current_if_file(g2, vf);
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
                                a20_envelope_race_test_note_pin_release(
                                    A20_ENVELOPE_RACE_OP_SCM,
                                    vf->identity);
#endif
                                vfs_put_file(vf);
                                ctrunc = 1;
                                continue;
                            }
                        }
                        c_rights_fds[c_rights_count] = g2;
                        c_rights_files[c_rights_count++] = vf;
                        fdout[delivered++] = g2;
                    }
                    if (delivered > 0) {
                        cmsg->cmsg_level = SOL_SOCKET;
                        cmsg->cmsg_type = SCM_RIGHTS;
                        cmsg->cmsg_len =
                            cmsg_len((size_t)delivered * sizeof(int));
                        size_t remaining = mh.msg_controllen - used;
                        size_t extent =
                            cmsg_space((size_t)delivered * sizeof(int));
                        used += extent < remaining ? extent : remaining;
                    } else {
                        ctrunc = 1;
                    }
                    if (fit < meta.scm_nfiles)
                        ctrunc = 1;
                    for (int i = fit; i < meta.scm_nfiles; i++) {
                        vfs_put_file(meta.scm_files[i]);
                        meta.scm_files[i] = NULL;
                    }
                }
                net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
#endif
            }
            if (cbuf) {
                if (used > 0 && copy_to_user(mh.msg_control, cbuf, used) < 0) {
                    kfree(cbuf);
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
                    a20_linux_scm_rights_recv_abort(
                        &rights_txn, NULL, linux_scm_close_if_file,
                        linux_scm_release_file);
#else
                    linux_scm_c_recv_abort(c_rights_fds, c_rights_files,
                                           &c_rights_count);
#endif
                    kfree(iov);
                    return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
                }
                kfree(cbuf);
            }
            mh.msg_controllen = used;
            if (copy_to_user(&((socket_msghdr_t *)msg)->msg_controllen,
                             &mh.msg_controllen, sizeof(mh.msg_controllen)) < 0) {
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
                a20_linux_scm_rights_recv_abort(
                    &rights_txn, NULL, linux_scm_close_if_file,
                    linux_scm_release_file);
#else
                linux_scm_c_recv_abort(c_rights_fds, c_rights_files,
                                       &c_rights_count);
#endif
                kfree(iov);
                return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
            }
        } else if (meta.scm_nfiles > 0) {
            /* No control buffer at all: received fds are closed. */
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
            int rr = a20_linux_scm_rights_recv_begin(
                (void **)meta.scm_files, meta.scm_nfiles, 0, 0,
                (flags & MSG_CMSG_CLOEXEC) != 0, NULL,
                linux_scm_install_recv_file, linux_scm_release_file,
                &rights_txn, NULL, &(size_t){0});
            meta.scm_nfiles = 0;
            if (rr < 0) {
                a20_linux_scm_rights_recv_abort(
                    &rights_txn, NULL, linux_scm_close_if_file,
                    linux_scm_release_file);
                kfree(iov);
                return linux_socket_unpin_result(sock, sock_vf, rr);
            }
            ctrunc = 1;
            a20_linux_scm_rights_recv_commit(
                &rights_txn, NULL, linux_scm_release_file);
#else
            net_scm_drop_files(meta.scm_files, meta.scm_nfiles);
            ctrunc = 1;
#endif
        }
        mh.msg_flags = ctrunc ? MSG_CTRUNC : 0;
        if (copy_to_user(&((socket_msghdr_t *)msg)->msg_flags,
                         &mh.msg_flags, sizeof(mh.msg_flags)) < 0) {
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
            a20_linux_scm_rights_recv_abort(
                &rights_txn, NULL, linux_scm_close_if_file,
                linux_scm_release_file);
#else
            linux_scm_c_recv_abort(c_rights_fds, c_rights_files,
                                   &c_rights_count);
#endif
            kfree(iov);
            return linux_socket_unpin_result(sock, sock_vf, -EFAULT);
        }
#ifdef CONFIG_LINUX_SCM_RIGHTS_RUST
        a20_linux_scm_rights_recv_commit(
            &rights_txn, NULL, linux_scm_release_file);
#else
        linux_scm_c_recv_commit(c_rights_files, &c_rights_count);
#endif
    }
    kfree(iov);
    return linux_socket_unpin_result(sock, sock_vf, r);
}

static int64_t linux_recvmsg_with_socket(int fd, void *msg, int flags,
                                         net_socket_t *pinned_sock,
                                         vfile_t *pinned_vf,
                                         uint64_t deadline)
{
    return linux_recvmsg_with_socket_header(fd, msg, NULL, flags,
                                            pinned_sock, pinned_vf, deadline);
}

int64_t sys_recvmsg(int fd, void *msg, int flags)
{
    return linux_recvmsg_with_socket(fd, msg, flags, NULL, NULL, 0);
}

int64_t sys_sendmmsg(int fd, void *mmsg, unsigned vlen, int flags)
{
    vfile_t *sock_vf = NULL;
    net_socket_t *sock = NULL;
    int pin = net_socket_pin_current_fd(fd, &sock_vf, &sock);
    if (pin < 0)
        return pin;
    if (vlen == 0)
        return linux_socket_unpin_result(sock, sock_vf, 0);
#ifdef CONFIG_LINUX_MMSG_TXN_RUST
    linux_mmsg_adapter_t ctx = {
        .fd = fd, .sock = sock, .sock_vf = sock_vf,
        .current_user_header = NULL,
    };
    int64_t result = a20_linux_sendmmsg_transaction(
        &ctx, mmsg, vlen, flags, sizeof(socket_msghdr_t), &linux_mmsg_ops);
    return linux_socket_unpin_result(sock, sock_vf, result);
#else
    unsigned total = vlen > 1024 ? 1024 : vlen;
    unsigned sent = 0;
    for (; sent < total; ) {
        uintptr_t base = (uintptr_t)mmsg;
        size_t offset = (size_t)sent * sizeof(socket_mmsghdr_t);
        if (offset > UINTPTR_MAX - base)
            return linux_socket_unpin_result(sock, sock_vf,
                                             sent ? (int64_t)sent : -EFAULT);
        socket_msghdr_t header;
        if (copy_from_user(&header, (void *)(base + offset), sizeof(header)) < 0)
            return linux_socket_unpin_result(sock, sock_vf,
                                             sent ? (int64_t)sent : -EFAULT);
        int64_t r = linux_sendmsg_with_socket(fd, &header, flags,
                                              sock, sock_vf);
        if (r < 0)
            return linux_socket_unpin_result(sock, sock_vf,
                                             sent ? (int64_t)sent : r);
        uint32_t msg_len = (uint32_t)r;
        size_t len_offset = offset + offsetof(socket_mmsghdr_t, msg_len);
        if (len_offset < offset || len_offset > UINTPTR_MAX - base ||
            copy_to_user((void *)(base + len_offset), &msg_len,
                         sizeof(msg_len)) < 0)
            return linux_socket_unpin_result(sock, sock_vf,
                                             sent ? (int64_t)sent : -EFAULT);
        sent++;
    }
    return linux_socket_unpin_result(sock, sock_vf, sent);
#endif
}

static int64_t linux_recvmmsg_abi(int fd, void *mmsg, unsigned vlen,
                                  int flags, void *timeout, int time64)
{
    vfile_t *sock_vf = NULL;
    net_socket_t *sock = NULL;
    int pin = net_socket_pin_current_fd(fd, &sock_vf, &sock);
    if (pin < 0)
        return pin;
#ifdef CONFIG_LINUX_MMSG_TXN_RUST
    linux_mmsg_adapter_t ctx = {
        .fd = fd, .sock = sock, .sock_vf = sock_vf,
        .current_user_header = NULL,
        .timeout_time64 = time64,
    };
    int64_t result = a20_linux_recvmmsg_transaction(
        &ctx, mmsg, vlen, flags, timeout, sizeof(socket_msghdr_t),
        &linux_mmsg_ops);
    return linux_socket_unpin_result(sock, sock_vf, result);
#else
    if (vlen == 0)
        return linux_socket_unpin_result(sock, sock_vf, 0);
    unsigned total = vlen > 1024 ? 1024 : vlen;
    uint64_t deadline = 0;
    if (timeout) {
        int tr = socket_timespec_to_deadline(timeout, time64, &deadline);
        if (tr < 0)
            return linux_socket_unpin_result(sock, sock_vf, tr);
    }
    unsigned recvd = 0;
    int64_t result = 0;
    for (; recvd < total; ) {
        int one_flags = flags;
        if (recvd > 0 && (flags & MSG_WAITFORONE))
            one_flags |= MSG_DONTWAIT;
        uintptr_t base = (uintptr_t)mmsg;
        size_t offset = (size_t)recvd * sizeof(socket_mmsghdr_t);
        if (offset > UINTPTR_MAX - base) {
            result = recvd ? (int64_t)recvd : -EFAULT;
            break;
        }
        int64_t r = linux_recvmsg_with_socket(
            fd, (void *)(base + offset), one_flags,
            sock, sock_vf, deadline);
        if (r < 0) {
            result = recvd ? (int64_t)recvd : r;
            break;
        }
        uint32_t len = (uint32_t)r;
        size_t len_offset = offset + offsetof(socket_mmsghdr_t, msg_len);
        if (len_offset < offset || len_offset > UINTPTR_MAX - base ||
            copy_to_user((void *)(base + len_offset), &len, sizeof(len)) < 0) {
            result = recvd ? (int64_t)recvd : -EFAULT;
            break;
        }
        recvd++;
        result = recvd;
        if (flags & MSG_WAITFORONE)
            flags |= MSG_DONTWAIT;
    }
    if (timeout) {
        int tr = socket_timespec_write_remaining(timeout, deadline, time64);
        if (tr < 0)
            result = tr;
    }
    return linux_socket_unpin_result(sock, sock_vf, result);
#endif
}

/* The legacy syscall uses the target's native time_t wire format. On
 * 32-bit targets that is two signed 32-bit fields; on 64-bit targets it is
 * the 64-bit timespec layout. The explicit time64 syscall always uses two
 * signed 64-bit fields. */
int64_t sys_recvmmsg(int fd, void *mmsg, unsigned vlen, int flags, void *timeout)
{
    return linux_recvmmsg_abi(fd, mmsg, vlen, flags, timeout,
                              sizeof(long) >= sizeof(int64_t));
}

int64_t sys_recvmmsg_time64(int fd, void *mmsg, unsigned vlen, int flags,
                            void *timeout)
{
    return linux_recvmmsg_abi(fd, mmsg, vlen, flags, timeout, 1);
}
