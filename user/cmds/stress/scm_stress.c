#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stddef.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0x40000000
#endif

#ifndef SCM_CREDENTIALS
#define SCM_CREDENTIALS 0x02
#endif
#ifndef SO_PASSCRED
#define SO_PASSCRED 16
#endif
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif

#ifndef HAVE_STRUCT_UCRED
#if defined(__GLIBC__) || defined(_GNU_SOURCE)
#define HAVE_STRUCT_UCRED 1
#endif
#endif
#ifndef HAVE_STRUCT_UCRED
struct ucred {
    int pid;
    int uid;
    int gid;
};
#endif

static int fail(const char *what) {
    printf("SCM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int send_fds(int sock, const int *fds, int nfds, const void *data,
                    size_t datalen, int flags) {
    char cbuf[CMSG_SPACE((size_t)nfds * sizeof(int))];
    struct iovec iov = {.iov_base = (void *)data, .iov_len = datalen};
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, (size_t)nfds * sizeof(int));
    return (int)sendmsg(sock, &mh, flags);
}

/* Returns number of received fds, or negative errno-style failure. */
static int recv_fds_capacity(int sock, int *fds, int maxfds, void *data,
                             size_t datalen, int flags, int *msg_flags,
                             size_t control_capacity) {
    char cbuf[CMSG_SPACE(16 * sizeof(int))];
    if (control_capacity > sizeof(cbuf))
        return -1;
    struct iovec iov = {.iov_base = data, .iov_len = datalen};
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = control_capacity;
    ssize_t n = recvmsg(sock, &mh, flags);
    if (n < 0)
        return -1;
    if (msg_flags)
        *msg_flags = mh.msg_flags;
    int got = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mh); cmsg;
         cmsg = CMSG_NXTHDR(&mh, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t payload = cmsg->cmsg_len - CMSG_LEN(0);
            size_t count = payload / sizeof(int);
            if (count > (size_t)maxfds)
                return -1;
            memcpy(fds, CMSG_DATA(cmsg), payload);
            got = (int)count;
        }
    }
    return got;
}

static int recv_fds(int sock, int *fds, int maxfds, void *data, size_t datalen,
                    int flags, int *msg_flags) {
    return recv_fds_capacity(sock, fds, maxfds, data, datalen, flags,
                             msg_flags, CMSG_SPACE((size_t)maxfds * sizeof(int)));
}

/* Bypass libc's ancillary-message conversions and inspect the Linux wire.
 * In particular, 64-bit length fields must be fully written and validated. */
typedef struct {
    void *name;
    unsigned namelen;
    struct iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
} raw_msghdr_t;

typedef struct {
    size_t len;
    int level, type;
} raw_cmsghdr_t;

static int raw_control_test(int fd) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair))
        return -1;
    union {
        size_t align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    raw_cmsghdr_t *header = (void *)control.bytes;
    header->len = CMSG_LEN(sizeof(int));
    header->level = SOL_SOCKET;
    header->type = SCM_RIGHTS;
    memcpy(control.bytes + CMSG_LEN(0), &fd, sizeof(fd));
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    raw_msghdr_t message = {.iov = &iov,
                            .iovlen = 1,
                            .control = control.bytes,
                            .controllen = sizeof(control.bytes)};
    if (sizeof(size_t) > 4) {
        message.iovlen = (size_t)((UINT64_C(1) << 32) | 1);
        errno = 0;
        if (syscall(SYS_sendmsg, pair[0], &message, 0) != -1 || errno != EMSGSIZE)
            return -1;
        message.iovlen = 1;
        header->len |= (size_t)(UINT64_C(1) << 32);
        errno = 0;
        if (syscall(SYS_sendmsg, pair[0], &message, 0) != -1 || errno != EINVAL)
            return -1;
        header->len = CMSG_LEN(sizeof(int));
    }
    union {
        size_t align;
        char bytes[2 * CMSG_SPACE(sizeof(int))];
    } malformed;
    memset(&malformed, 0, sizeof(malformed));
    memcpy(malformed.bytes, control.bytes, sizeof(control.bytes));
    raw_cmsghdr_t *second = (void *)(malformed.bytes + sizeof(control.bytes));
    second->len = CMSG_LEN(sizeof(int));
    second->level = SOL_SOCKET;
    second->type = SCM_RIGHTS;
    message.control = malformed.bytes;
    message.controllen = sizeof(control.bytes) + sizeof(*second);
    errno = 0;
    if (syscall(SYS_sendmsg, pair[0], &message, 0) != -1 || errno != EINVAL)
        return -1;
    message.control = control.bytes;
    message.controllen = sizeof(control.bytes);
    for (int i = 0; i < 512; i++) {
        if (send_fds(pair[0], &fd, 1, &byte, 1, 0) != 1)
            return -1;
        memset(&control, 0xa5, sizeof(control));
        message.controllen = sizeof(control.bytes);
        message.flags = 0;
        if (syscall(SYS_recvmsg, pair[1], &message, MSG_CMSG_CLOEXEC) != 1 ||
            message.controllen != CMSG_SPACE(sizeof(int)) || (message.flags & ~MSG_CMSG_CLOEXEC) ||
            header->len != CMSG_LEN(sizeof(int)) ||
            header->level != SOL_SOCKET || header->type != SCM_RIGHTS)
            return -1;
        int received;
        memcpy(&received, control.bytes + CMSG_LEN(0), sizeof(received));
        if (fcntl(received, F_GETFD) != FD_CLOEXEC || close(received))
            return -1;
    }
    close(pair[0]);
    close(pair[1]);
    puts("SCM_STRESS: native-wire-lengths PASS");
    return 0;
}

/* Firefox's request/reply transport emits SCM_RIGHTS even when no handles
 * are attached. Linux accepts that header and sends the payload normally. */
static int empty_rights_test(int fd) {
    const int types[] = {SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int pair[2];
        if (socketpair(AF_UNIX, types[i], 0, pair))
            return -1;
        if (send_fds(pair[0], &fd, 0, "empty", 5, 0) != 5)
            return -1;
        char data[16] = {0};
        int received = -1, flags = 0;
        if (recv_fds(pair[1], &received, 1, data, sizeof(data), 0, &flags) != 0 ||
            memcmp(data, "empty", 5) || flags || received != -1)
            return -1;
        /* An empty header must not terminate parsing of later headers. */
        union {
            size_t align;
            char bytes[CMSG_SPACE(0) + CMSG_SPACE(sizeof(int))];
        } control = {0};
        struct iovec iov = {.iov_base = "mixed", .iov_len = 5};
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                            .msg_control = control.bytes,
                            .msg_controllen = sizeof(control.bytes)};
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(0);
        cmsg = CMSG_NXTHDR(&msg, cmsg);
        if (!cmsg)
            return -1;
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        if (sendmsg(pair[0], &msg, 0) != 5 ||
            recv_fds(pair[1], &received, 1, data, sizeof(data), 0, &flags) != 1 ||
            received < 0 || memcmp(data, "mixed", 5) || flags || close(received))
            return -1;
        close(pair[0]);
        close(pair[1]);
    }
    puts("SCM_STRESS: empty-rights PASS");
    return 0;
}


/* Connections and queued credentials belong to a process even when its
 * worker thread creates/sends through the socket and then exits. */
struct threaded_credentials {
    int type;
    int sv[2];
    int listener;
    struct sockaddr_un address;
    socklen_t length;
    int error;
};

static void *credential_worker(void *arg) {
    struct threaded_credentials *test = arg;
    if (test->listener >= 0) {
        test->sv[0] = socket(AF_UNIX, test->type, 0);
        if (test->sv[0] < 0 || connect(test->sv[0],
                (struct sockaddr *)&test->address, test->length) < 0)
            goto bad;
    } else if (socketpair(AF_UNIX, test->type, 0, test->sv) < 0) {
        goto bad;
    }
    int receiver = test->listener >= 0 ?
        accept(test->listener, NULL, NULL) : test->sv[1];
    if (receiver < 0)
        goto bad;
    test->sv[1] = receiver;
    int on = 1;
    if (setsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0 ||
        send(test->sv[0], "T", 1, 0) != 1)
        goto bad;
    return NULL;
bad:
    test->error = errno ? errno : EIO;
    return NULL;
}

static int threaded_credentials_test(void) {
    const int types[] = {SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        /* Also cover a worker connecting to a bound listening socket. */
        for (int connected = 0; connected <= (types[i] != SOCK_DGRAM); connected++) {
            struct threaded_credentials test = {
                .type = types[i], .sv = {-1, -1}, .listener = -1,
            };
            if (connected) {
                test.listener = socket(AF_UNIX, types[i], 0);
                test.address.sun_family = AF_UNIX;
                snprintf(test.address.sun_path + 1,
                         sizeof(test.address.sun_path) - 1,
                         "scm-process-%d-%u", getpid(), i);
                test.length = offsetof(struct sockaddr_un, sun_path) + 1 +
                              strlen(test.address.sun_path + 1);
                if (test.listener < 0 || bind(test.listener,
                        (struct sockaddr *)&test.address, test.length) < 0 ||
                    listen(test.listener, 1) < 0)
                    return fail("thread credential listener");
            }
            pthread_t thread;
            if (pthread_create(&thread, NULL, credential_worker, &test) ||
                pthread_join(thread, NULL) || test.error)
                return fail("thread credential worker");
            struct ucred peer = {0};
            socklen_t length = sizeof(peer);
            if (getsockopt(test.sv[1], SOL_SOCKET, SO_PEERCRED,
                           &peer, &length) < 0 || peer.pid != getpid())
                return fail("worker SO_PEERCRED must name process");
            char control[CMSG_SPACE(sizeof(peer))], data;
            struct iovec iov = {.iov_base = &data, .iov_len = 1};
            struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1,
                .msg_control = control, .msg_controllen = sizeof(control)};
            if (recvmsg(test.sv[1], &message, 0) != 1)
                return fail("worker credential receive");
            struct cmsghdr *header = CMSG_FIRSTHDR(&message);
            if (!header || header->cmsg_type != SCM_CREDENTIALS)
                return fail("worker missing SCM_CREDENTIALS");
            memcpy(&peer, CMSG_DATA(header), sizeof(peer));
            if (peer.pid != getpid())
                return fail("worker SCM_CREDENTIALS must name process");
            /* SO_PEERPIDFD (Linux 6.5+) must survive the worker's exit. */
            int pidfd = -1;
            length = sizeof(pidfd);
            if (getsockopt(test.sv[1], SOL_SOCKET, 77, &pidfd, &length) == 0) {
                struct pollfd pollfd = {.fd = pidfd, .events = POLLIN};
                if (pidfd <= 2 || !(fcntl(pidfd, F_GETFD) & FD_CLOEXEC) ||
                    poll(&pollfd, 1, 0) != 0)
                    return fail("worker peer pidfd must remain alive");
                close(pidfd);
            } else if (errno != ENOPROTOOPT) {
                return fail("worker peer pidfd");
            }
            close(test.sv[0]);
            close(test.sv[1]);
            if (test.listener >= 0)
                close(test.listener);
        }
    }
    puts("SCM_STRESS: process-credentials PASS");
    return 0;
}

int main(void) {
    if (threaded_credentials_test())
        return 1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("socketpair");

    int f = open("/scm_stress_tmp", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (f < 0)
        return fail("open tmp");
    char data[16];
    if (write(f, "hello", 5) != 5)
        return fail("write tmp");

    if (empty_rights_test(f))
        return fail("empty rights");

    /* A later invalid fd must roll back earlier staged refs and enqueue no
     * message. The following valid transfer proves the stream remains usable. */
    int invalid_pair[2] = {f, -1};
    errno = 0;
    if (send_fds(sv[0], invalid_pair, 2, "bad", 3, 0) != -1 || errno != EBADF)
        return fail("invalid send fd rollback");

    /* Aligned capacity must deliver a fitting prefix on every ABI.  Three
     * queued descriptors and room for two avoid the x86_64 CMSG_SPACE(1)==
     * CMSG_SPACE(2) alignment trap. */
    int three_for_prefix[3] = {f, f, f};
    if (send_fds(sv[0], three_for_prefix, 3, "P", 1, 0) != 1)
        return fail("send 3-fd prefix");
    int prefix[3] = {-1, -1, -1}, prefix_flags = 0;
    int prefix_got = recv_fds_capacity(sv[1], prefix, 3, data, sizeof(data), 0,
                                       &prefix_flags,
                                       CMSG_SPACE(2 * sizeof(int)));
    printf("SCM_STRESS: aligned prefix got=%d flags=0x%x fds=%d,%d,%d errno=%d\n",
           prefix_got, prefix_flags, prefix[0], prefix[1], prefix[2], errno);
    if (prefix_got != 2 || prefix[0] < 0 || prefix[1] < 0 || prefix[2] != -1 ||
        !(prefix_flags & MSG_CTRUNC))
        return fail("receive aligned 2-fd prefix with MSG_CTRUNC");
    if (close(prefix[0]) || close(prefix[1]))
        return fail("close aligned truncated prefix fds");

    /* The final cmsg may use its compact, unpadded CMSG_LEN capacity. */
    int two_for_compact[2] = {f, f};
    if (send_fds(sv[0], two_for_compact, 2, "Q", 1, 0) != 1)
        return fail("send compact prefix pair");
    int compact[2] = {-1, -1}, compact_flags = 0;
    int compact_got = recv_fds_capacity(sv[1], compact, 2, data, sizeof(data), 0,
                                        &compact_flags, CMSG_LEN(sizeof(int)));
    printf("SCM_STRESS: compact prefix got=%d flags=0x%x fds=%d,%d errno=%d\n",
           compact_got, compact_flags, compact[0], compact[1], errno);
    if (compact_got != 1 || compact[0] < 0 || compact[1] != -1 ||
        !(compact_flags & MSG_CTRUNC))
        return fail("receive compact CMSG_LEN prefix with MSG_CTRUNC");
    if (close(compact[0]))
        return fail("close compact truncated prefix fd");

    /* Single fd, single message. */
    if (send_fds(sv[0], &f, 1, "A", 1, 0) != 1)
        return fail("sendmsg 1 fd");

    int rfd = -1;
    int mflags = 0;
    int got = recv_fds(sv[1], &rfd, 1, data, sizeof(data), 0, &mflags);
    if (got != 1 || rfd < 0)
        return fail("recvmsg 1 fd");
    if (rfd == f)
        return fail("received fd must be a new number");
    if (mflags & MSG_CTRUNC)
        return fail("unexpected MSG_CTRUNC");

    if (lseek(rfd, 0, SEEK_SET) < 0)
        return fail("lseek received fd");
    char content[8] = {0};
    if (read(rfd, content, 5) != 5 || memcmp(content, "hello", 5) != 0)
        return fail("received fd reads same file");

    /* The shared open file description shares the file offset. */
    if (lseek(f, 0, SEEK_CUR) != 5)
        return fail("offset must be shared like dup");

    /* Two fds in one cmsg. */
    int pair[2] = {f, rfd};
    if (send_fds(sv[0], pair, 2, "B", 1, 0) != 1)
        return fail("sendmsg 2 fds");
    int out2[2] = {-1, -1};
    if (recv_fds(sv[1], out2, 2, data, sizeof(data), 0, NULL) != 2)
        return fail("recvmsg 2 fds");
    if (out2[0] < 0 || out2[1] < 0 || out2[0] == out2[1])
        return fail("2 fds distinct");
    close(out2[0]);
    close(out2[1]);

    /* Back-to-back messages, one recvmsg: both fds must arrive. */
    if (send_fds(sv[0], &f, 1, "C", 1, 0) != 1)
        return fail("sendmsg back-to-back 1");
    if (send_fds(sv[0], &f, 1, "D", 1, 0) != 1)
        return fail("sendmsg back-to-back 2");
    int out3[2] = {-1, -1};
    if (recv_fds(sv[1], out3, 2, data, sizeof(data), 0, NULL) != 2)
        return fail("coalesced recv 2 fds");
    if (out3[0] < 0 || out3[1] < 0)
        return fail("coalesced fds valid");
    close(out3[0]);
    close(out3[1]);

    /* MSG_CMSG_CLOEXEC accepted. */
    if (send_fds(sv[0], &f, 1, "E", 1, 0) != 1)
        return fail("sendmsg for cloexec");
    int cefd = -1;
    if (recv_fds(sv[1], &cefd, 1, data, sizeof(data), MSG_CMSG_CLOEXEC, NULL) !=
            1 || cefd < 0 || fcntl(cefd, F_GETFD) != FD_CLOEXEC)
        return fail("recvmsg MSG_CMSG_CLOEXEC");
    close(cefd);

    /* Sending fds over INET must fail with EOPNOTSUPP. */
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0)
        return fail("udp socket");
    errno = 0;
    if (send_fds(udp, &f, 1, "X", 1, MSG_DONTWAIT) >= 0 || errno != EOPNOTSUPP)
        return fail("INET SCM_RIGHTS must be EOPNOTSUPP");
    close(udp);

    /* SCM_CREDENTIALS: with SO_PASSCRED the receiver must get the sender
     * pid/uid/gid via SCM_CREDENTIALS cmsg; SO_PEERCRED must agree. */
    int cv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cv) < 0)
        return fail("socketpair cred");
    int opt = 1;
    if (setsockopt(cv[1], SOL_SOCKET, SO_PASSCRED, &opt, sizeof(opt)) < 0)
        return fail("setsockopt SO_PASSCRED");
    char msg[] = "cred";
    struct iovec iov0 = {.iov_base = msg, .iov_len = sizeof(msg)};
    struct msghdr smh;
    memset(&smh, 0, sizeof(smh));
    smh.msg_iov = &iov0;
    smh.msg_iovlen = 1;
    if (sendmsg(cv[0], &smh, 0) != (ssize_t)sizeof(msg))
        return fail("sendmsg cred");

    char cbuf2[CMSG_SPACE(sizeof(struct ucred))];
    char rbuf[16];
    struct iovec iov1 = {.iov_base = rbuf, .iov_len = sizeof(rbuf)};
    struct msghdr rmh;
    memset(&rmh, 0, sizeof(rmh));
    rmh.msg_iov = &iov1;
    rmh.msg_iovlen = 1;
    rmh.msg_control = cbuf2;
    rmh.msg_controllen = sizeof(cbuf2);
    ssize_t cr = recvmsg(cv[1], &rmh, 0);
    if (cr != (ssize_t)sizeof(msg))
        return fail("recvmsg cred");
    struct ucred peer;
    int found_cred = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&rmh); c; c = CMSG_NXTHDR(&rmh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
            memcpy(&peer, CMSG_DATA(c), sizeof(peer));
            found_cred = 1;
        }
    }
    if (!found_cred)
        return fail("missing SCM_CREDENTIALS cmsg");
    if (peer.pid <= 0 || peer.uid > 65535)
        return fail("SCM_CREDENTIALS bogus values");

    struct ucred pc;
    socklen_t pcl = sizeof(pc);
    if (getsockopt(cv[1], SOL_SOCKET, SO_PEERCRED, &pc, &pcl) < 0)
        return fail("getsockopt SO_PEERCRED");
    if (pc.pid != peer.pid || pc.uid != peer.uid || pc.gid != peer.gid)
        return fail("SO_PEERCRED mismatch");

    close(cv[0]);
    close(cv[1]);

    if (raw_control_test(f))
        return fail("raw ancillary wire layout");

    close(rfd);
    close(f);
    unlink("/scm_stress_tmp");
    close(sv[0]);
    close(sv[1]);

    printf("SCM_STRESS: PASS\n");
    return 0;
}
