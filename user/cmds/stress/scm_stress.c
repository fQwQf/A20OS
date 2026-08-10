#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

static int fail(const char *what)
{
    printf("SCM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int send_fds(int sock, const int *fds, int nfds, const void *data,
                    size_t datalen, int flags)
{
    char cbuf[CMSG_SPACE((size_t)nfds * sizeof(int))];
    struct iovec iov = { .iov_base = (void *)data, .iov_len = datalen };
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
static int recv_fds(int sock, int *fds, int maxfds, void *data,
                    size_t datalen, int flags, int *msg_flags)
{
    char cbuf[CMSG_SPACE((size_t)maxfds * sizeof(int))];
    struct iovec iov = { .iov_base = data, .iov_len = datalen };
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
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

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return fail("socketpair");

    int f = open("/scm_stress_tmp", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (f < 0)
        return fail("open tmp");
    if (write(f, "hello", 5) != 5)
        return fail("write tmp");

    /* Single fd, single message. */
    if (send_fds(sv[0], &f, 1, "A", 1, 0) != 1)
        return fail("sendmsg 1 fd");

    int rfd = -1;
    char data[16];
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
    int pair[2] = { f, rfd };
    if (send_fds(sv[0], pair, 2, "B", 1, 0) != 1)
        return fail("sendmsg 2 fds");
    int out2[2] = { -1, -1 };
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
    int out3[2] = { -1, -1 };
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
    if (recv_fds(sv[1], &cefd, 1, data, sizeof(data), MSG_CMSG_CLOEXEC,
                 NULL) != 1 || cefd < 0)
        return fail("recvmsg MSG_CMSG_CLOEXEC");
    close(cefd);

    /* Sending fds over INET must fail with EOPNOTSUPP. */
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0)
        return fail("udp socket");
    errno = 0;
    if (send_fds(udp, &f, 1, "X", 1, MSG_DONTWAIT) >= 0 ||
        errno != EOPNOTSUPP)
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
    struct iovec iov0 = { .iov_base = msg, .iov_len = sizeof(msg) };
    struct msghdr smh;
    memset(&smh, 0, sizeof(smh));
    smh.msg_iov = &iov0;
    smh.msg_iovlen = 1;
    if (sendmsg(cv[0], &smh, 0) != (ssize_t)sizeof(msg))
        return fail("sendmsg cred");

    char cbuf2[CMSG_SPACE(sizeof(struct ucred))];
    char rbuf[16];
    struct iovec iov1 = { .iov_base = rbuf, .iov_len = sizeof(rbuf) };
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
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&rmh); c;
         c = CMSG_NXTHDR(&rmh, c)) {
        if (c->cmsg_level == SOL_SOCKET &&
            c->cmsg_type == SCM_CREDENTIALS) {
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

    close(rfd);
    close(f);
    unlink("/scm_stress_tmp");
    close(sv[0]);
    close(sv[1]);

    printf("SCM_STRESS: PASS\n");
    return 0;
}
