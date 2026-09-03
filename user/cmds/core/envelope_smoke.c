/* Capability envelope attack suite (docs/research/05 §2.5, 11 §4.6).
 *
 * Runs as supervisor: creates envelopes, forks workers that enter() an
 * envelope and then attempt legitimate work plus policy violations.
 * Each scenario prints "ENVELOPE_SMOKE: <name> PASS" on success.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903
#define SYS_a20_envelope_revoke 904
#define SYS_a20_envelope_stats  905
#define SYS_a20_envelope_audit  906
#define SYS_a20_channel_pair    900
#define SYS_io_uring_setup      425

/* Must match kernel/include/ipc/ipc.h object types and right bits. */
#define ENV_OBJ_FILE    3
#define ENV_OBJ_SOCKET  5
#define ENV_OBJ_EVENT_QUEUE 8
#define ENV_R_READ      (1ull << 0)
#define ENV_R_WRITE     (1ull << 1)
#define ENV_R_STAT      (1ull << 3)
#define ENV_R_SEEK      (1ull << 4)
#define ENV_R_CONNECT   (1ull << 9)
#define ENV_R_ACCEPT    (1ull << 10)
#define ENV_F_KILL      (1u << 0)

struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

static long env_create(struct env_policy *p)
{
    return syscall(SYS_a20_envelope_create, p, 0L);
}
static long env_enter(long id)
{
    return syscall(SYS_a20_envelope_enter, id);
}
static long env_revoke(long id)
{
    return syscall(SYS_a20_envelope_revoke, id);
}

static void policy_file_rw(struct env_policy *p)
{
    memset(p, 0, sizeof(*p));
    p->allowed_types = 1u << ENV_OBJ_FILE;
    p->rights_by_class[ENV_OBJ_FILE] =
        ENV_R_READ | ENV_R_WRITE | ENV_R_STAT | ENV_R_SEEK;
    p->op_budget = 1000;
    p->data_budget = 1 << 20;
}

static int expect_errno(long rc, int want, const char *what)
{
    if (rc == -1 && errno == want)
        return 0;
    printf("ENVELOPE_SMOKE: %s got rc=%ld errno=%d want errno=%d\n",
           what, rc, errno, want);
    return 1;
}

static void die(int code)
{
    printf("ENVELOPE_SMOKE: SCENARIO FAIL code=%d\n", code);
    exit(code);
}

/* T1: enveloped process does normal file work. */
static int t1_functional(long env_id)
{
    if (env_enter(env_id) < 0)
        die(11);

    int fd = open("/tmp/env_t1.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        die(12);
    const char *msg = "enveloped-ok";
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg))
        die(13);
    if (lseek(fd, 0, SEEK_SET) < 0)
        die(14);
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n != (ssize_t)strlen(msg) || memcmp(buf, msg, n) != 0)
        die(15);
    close(fd);
    return 0;
}

/* T2: socket acquisition outside allowed_types must fail with EPERM,
 * while files stay usable inside the same envelope. */
static int t2_type_deny(long env_id)
{
    if (env_enter(env_id) < 0)
        die(21);

    long s = socket(AF_INET, SOCK_STREAM, 0);
    if (expect_errno(s, EPERM, "type-deny socket"))
        die(22);

    int sv[2];
    if (expect_errno(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), EPERM,
                     "type-deny socketpair"))
        die(24);

    int ch[2];
    if (expect_errno(syscall(SYS_a20_channel_pair, ch), EPERM,
                     "type-deny channel-pair"))
        die(25);

    unsigned char ring_params[120];
    memset(ring_params, 0, sizeof(ring_params));
    if (expect_errno(syscall(SYS_io_uring_setup, 8, ring_params), EPERM,
                     "type-deny io-uring"))
        die(26);

    int fd = open("/tmp/env_t2.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        die(23);
    close(fd);
    return 0;
}

/* T3: open(O_WRONLY) against a read-only class cap fails with EACCES. */
static int t3_rights_deny(long env_id)
{
    if (env_enter(env_id) < 0)
        die(31);

    long fd = open("/tmp/env_t3.txt", O_CREAT | O_WRONLY, 0644);
    if (expect_errno(fd, EACCES, "rights-deny open"))
        die(32);

    int rd = open("/tmp/env_t3.txt", O_RDONLY);
    if (rd < 0)
        die(33);
    close(rd);
    return 0;
}

/* T4: op budget exhaustion denies the third mediated operation. */
static int t4_op_budget(long env_id)
{
    if (env_enter(env_id) < 0)
        die(41);

    int fd = open("/tmp/env_t4.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        die(42);
    if (write(fd, "ab", 2) != 2)
        die(43);
    char b[4];
    long r = read(fd, b, sizeof(b));
    if (expect_errno(r, EACCES, "op-budget read"))
        die(44);
    close(fd);
    return 0;
}

/* T5: data budget charges bytes; over-budget writes are denied. */
static int t5_data_budget(long env_id)
{
    if (env_enter(env_id) < 0)
        die(51);

    int fd = open("/tmp/env_t5.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        die(52);
    char big[16];
    memset(big, 'x', sizeof(big));
    long r = write(fd, big, sizeof(big));
    if (expect_errno(r, EACCES, "data-budget write"))
        die(53);
    if (write(fd, "ok", 2) != 2)
        die(54);
    long r2 = write(fd, "12345678", 8);
    if (expect_errno(r2, EACCES, "data-budget second"))
        die(55);
    close(fd);
    return 0;
}

/* T6: expired time budget denies later operations. */
static int t6_expiry(long env_id)
{
    if (env_enter(env_id) < 0)
        die(61);

    int fd = open("/tmp/env_t6.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        die(62);
    if (write(fd, "early", 5) != 5)
        die(63);
    sleep(1);
    char b[8];
    long r = read(fd, b, sizeof(b));
    if (expect_errno(r, EACCES, "expiry read"))
        die(64);
    close(fd);
    return 0;
}

/* T8: reopen through /proc/self/fd cannot upgrade authority (05 §2.5.1 A8):
 * the reopened descriptor inherits the intersection of the requested mode
 * with the source shadow, so a write on it must be denied. */
static int t8_reopen(long env_id)
{
    if (env_enter(env_id) < 0)
        die(81);
    int rd = open("/tmp/env_t8.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (rd < 0)
        die(82);
    if (write(rd, "x", 1) != 1)
        die(83);
    int ro = open("/tmp/env_t8.txt", O_RDONLY);
    if (ro < 0)
        die(87);
    char lp[64];
    snprintf(lp, sizeof(lp), "/proc/self/fd/%d", ro);
    int up = open(lp, O_RDWR);
    if (up < 0)
        die(84);
    long r = write(up, "y", 1);
    if (expect_errno(r, EACCES, "reopen-upgrade write"))
        die(85);
    if (lseek(up, 0, SEEK_SET) < 0)
        die(86);
    close(up);
    close(ro);
    close(rd);
    return 0;
}

/* T9: socket data plane -- byte budgets charge on sendto/recvfrom for
 * tracked socketpair descriptors, and in-budget traffic moves end-to-end
 * (05 §2.5.1 USE surface).  An AF_UNIX pair keeps the
 * test deterministic: no NIC needed, peers pre-connected; over-budget
 * sends must fail EACCES inside the mediator BEFORE any dispatch. */
static int t9_net_data_plane(long env_id)
{
    static char payload[(1 << 20) + 16]; /* exceeds 1 MiB data budget */
    static char rxbuf[64];

    if (env_enter(env_id) < 0)
        die(81);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        die(82);

    if (expect_errno(sendto(sv[0], payload, sizeof(payload), 0, NULL, 0),
                     EACCES, "net over-budget sendto"))
        die(83);

    long r = sendto(sv[0], payload, 16, 0, NULL, 0);
    if (r != 16)
        die(84);

    r = recvfrom(sv[1], rxbuf, sizeof(rxbuf), 0, NULL, NULL);
    if (r != 16)
        die(85);

    unsigned int ring_params[30]; /* Linux io_uring_params is 120 bytes. */
    memset(ring_params, 0, sizeof(ring_params));
    int ringfd = syscall(SYS_io_uring_setup, 8, ring_params);
    if (ringfd < 0)
        die(86);
    close(ringfd);
    memset(ring_params, 0, sizeof(ring_params));
    ring_params[2] = 1u << 1; /* IORING_SETUP_SQPOLL */
    if (expect_errno(syscall(SYS_io_uring_setup, 8, ring_params), EPERM,
                     "io-uring SQPOLL"))
        die(87);

    close(sv[0]);
    close(sv[1]);
    return 0;
}

int main(void)
{
    printf("ENVELOPE_SMOKE: start\n");

    int failures = 0;
    long ids[8];
    struct env_policy p;

    policy_file_rw(&p);
    ids[0] = env_create(&p);
    ids[1] = env_create(&p);
    policy_file_rw(&p);
    p.rights_by_class[ENV_OBJ_FILE] = ENV_R_READ | ENV_R_STAT | ENV_R_SEEK;
    ids[2] = env_create(&p);
    policy_file_rw(&p);
    p.op_budget = 2;
    ids[3] = env_create(&p);
    policy_file_rw(&p);
    p.data_budget = 8;
    ids[4] = env_create(&p);
    policy_file_rw(&p);
    p.time_budget_ns = 300ULL * 1000 * 1000;
    ids[5] = env_create(&p);
    policy_file_rw(&p);
    ids[6] = env_create(&p);
    policy_file_rw(&p);
    p.allowed_types |= 1u << ENV_OBJ_SOCKET;
    p.allowed_types |= 1u << ENV_OBJ_EVENT_QUEUE;
    p.rights_by_class[ENV_OBJ_SOCKET] =
        ENV_R_READ | ENV_R_WRITE | ENV_R_STAT | ENV_R_CONNECT | ENV_R_ACCEPT;
    p.rights_by_class[ENV_OBJ_EVENT_QUEUE] =
        ENV_R_READ | ENV_R_WRITE | ENV_R_STAT;
    ids[7] = env_create(&p);
    for (int i = 0; i < 8; i++)
        if (ids[i] < 0) {
            printf("ENVELOPE_SMOKE: create %d failed\n", i);
            return 1;
        }

    int (*scenarios[8])(long) = {
        t1_functional, t2_type_deny, t3_rights_deny,
        t4_op_budget, t5_data_budget, t6_expiry, t8_reopen,
        t9_net_data_plane,
    };
    const char *names[8] = {
        "functional", "type-deny", "rights-deny",
        "op-budget", "data-budget", "expiry", "reopen-upgrade",
        "net-data-plane",
    };

    for (int i = 0; i < 8; i++) {
        pid_t c = fork();
        if (c < 0) {
            printf("ENVELOPE_SMOKE: fork failed\n");
            return 1;
        }
        if (c == 0)
            exit(scenarios[i](ids[i]));
        int st = 0;
        waitpid(c, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
            printf("ENVELOPE_SMOKE: %s PASS\n", names[i]);
        else
            failures++;
    }

    /* T7: active revocation with KILL_ON_EXPIRE kills every attached worker,
     * including a population larger than the mediator's internal batch. */
    policy_file_rw(&p);
    p.flags = ENV_F_KILL;
    long kid = env_create(&p);
    int sync_pipe[2];
    if (kid < 0 || pipe(sync_pipe) < 0)
        return 1;
    enum { KILL_WORKERS = 40 };
    pid_t workers[KILL_WORKERS];
    for (int i = 0; i < KILL_WORKERS; i++) {
        workers[i] = fork();
        if (workers[i] < 0)
            return 1;
        if (workers[i] == 0) {
            close(sync_pipe[0]);
            if (env_enter(kid) < 0)
                _exit(71);
            char ready = 'R';
            if (write(sync_pipe[1], &ready, 1) != 1)
                _exit(73);
            for (;;)
                pause();
        }
    }
    close(sync_pipe[1]);
    for (int i = 0; i < KILL_WORKERS; i++) {
        char ready;
        if (read(sync_pipe[0], &ready, 1) != 1)
            return 1;
    }
    long rr = env_revoke(kid);
    int killed = 0;
    for (int i = 0; i < KILL_WORKERS; i++) {
        int st = 0;
        waitpid(workers[i], &st, 0);
        if (WIFSIGNALED(st) && WTERMSIG(st) == 9)
            killed++;
    }
    close(sync_pipe[0]);
    if (rr == 0 && killed == KILL_WORKERS) {
        printf("ENVELOPE_SMOKE: revoke-kill PASS\n");
    } else {
        printf("ENVELOPE_SMOKE: revoke-kill FAIL rr=%ld killed=%d/%d\n",
               rr, killed, KILL_WORKERS);
        failures++;
    }

    /* ---- Bespoke escape-surface blocks (need parent cooperation) ---- */

    {
        /* T9: SCM_RIGHTS receive — an enveloped receiver obtains the fd
         * through mediation and it stays usable (05 §2.5.3). */
        struct env_policy p9;
        policy_file_rw(&p9);
        long id9 = env_create(&p9);
        int sp9[2];
        int syn9[2];
        if (id9 < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp9) < 0 ||
            pipe(syn9) < 0)
            return 1;
        pid_t c9 = fork();
        if (c9 == 0) {
            close(sp9[0]);
            close(syn9[0]);
            if (env_enter(id9) < 0)
                _exit(91);
            char rdy = 'R';
            if (write(syn9[1], &rdy, 1) != 1)
                _exit(92);
            char data[32];
            struct iovec iov = { data, sizeof(data) };
            union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u;
            memset(u.buf, 0, sizeof(u.buf));
            struct msghdr mh;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = u.buf;
            mh.msg_controllen = sizeof(u.buf);
            ssize_t n = recvmsg(sp9[1], &mh, 0);
            if (n < 0)
                _exit(93);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            if (!cm || cm->cmsg_len != CMSG_LEN(sizeof(int)))
                _exit(94);
            int fdr = *(int *)CMSG_DATA(cm);
            char b[1];
            if (read(fdr, b, 1) != 1)
                _exit(95);
            close(fdr);
            _exit(0);
        }
        close(sp9[1]);
        close(syn9[1]);
        char rdy9;
        if (read(syn9[0], &rdy9, 1) != 1)
            return 1;
        int pfd9 = open("/tmp/env_scm.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (pfd9 < 0 || write(pfd9, "z", 1) != 1)
            return 1;
        (void)lseek(pfd9, 0, SEEK_SET); /* receiver shares the offset */
        struct iovec iov9 = { (void *)"f", 1 };
        union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u9;
        memset(u9.buf, 0, sizeof(u9.buf));
        struct msghdr mh9;
        memset(&mh9, 0, sizeof(mh9));
        mh9.msg_iov = &iov9;
        mh9.msg_iovlen = 1;
        mh9.msg_control = u9.buf;
        mh9.msg_controllen = sizeof(u9.buf);
        struct cmsghdr *cm9 = CMSG_FIRSTHDR(&mh9);
        cm9->cmsg_level = SOL_SOCKET;
        cm9->cmsg_type = SCM_RIGHTS;
        cm9->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm9), &pfd9, sizeof(int));
        ssize_t sn9 = sendmsg(sp9[0], &mh9, 0);
        int st9 = 0;
        waitpid(c9, &st9, 0);
        close(pfd9);
        close(sp9[0]);
        close(syn9[0]);
        if (sn9 >= 0 && WIFEXITED(st9) && WEXITSTATUS(st9) == 0) {
            printf("ENVELOPE_SMOKE: scm-receive PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: scm-receive FAIL sn=%ld st=%d\n",
                   (long)sn9, st9);
            failures++;
        }
    }

    {
        /* T10: SCM_RIGHTS receive of a class outside allowed_types drops
         * the descriptor instead of installing it. */
        struct env_policy pa;
        memset(&pa, 0, sizeof(pa));
        pa.allowed_types = 1u << ENV_OBJ_EVENT_QUEUE;
        pa.rights_by_class[ENV_OBJ_EVENT_QUEUE] =
            ENV_R_READ | ENV_R_STAT;
        pa.op_budget = 100;
        pa.data_budget = 4096;
        long id10 = env_create(&pa);
        int sp10[2];
        int syn10[2];
        if (id10 < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp10) < 0 ||
            pipe(syn10) < 0)
            return 1;
        pid_t c10 = fork();
        if (c10 == 0) {
            close(sp10[0]);
            close(syn10[0]);
            if (env_enter(id10) < 0)
                _exit(96);
            char rdy = 'R';
            if (write(syn10[1], &rdy, 1) != 1)
                _exit(97);
            char data[32];
            struct iovec iov = { data, sizeof(data) };
            union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u;
            memset(u.buf, 0, sizeof(u.buf));
            struct msghdr mh;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = u.buf;
            mh.msg_controllen = sizeof(u.buf);
            ssize_t n = recvmsg(sp10[1], &mh, 0);
            if (n < 0)
                _exit(98);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            if (cm && cm->cmsg_type == SCM_RIGHTS &&
                cm->cmsg_len == CMSG_LEN(sizeof(int)))
                _exit(99); /* a denied descriptor must not be installed */
            _exit(0);
        }
        close(sp10[1]);
        close(syn10[1]);
        char rdy10;
        if (read(syn10[0], &rdy10, 1) != 1)
            return 1;
        int pfd10 = open("/tmp/env_scm2.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (pfd10 < 0 || write(pfd10, "z", 1) != 1)
            return 1;
        struct iovec iov10 = { (void *)"f", 1 };
        union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u10;
        memset(u10.buf, 0, sizeof(u10.buf));
        struct msghdr mh10;
        memset(&mh10, 0, sizeof(mh10));
        mh10.msg_iov = &iov10;
        mh10.msg_iovlen = 1;
        mh10.msg_control = u10.buf;
        mh10.msg_controllen = sizeof(u10.buf);
        struct cmsghdr *cm10 = CMSG_FIRSTHDR(&mh10);
        cm10->cmsg_level = SOL_SOCKET;
        cm10->cmsg_type = SCM_RIGHTS;
        cm10->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm10), &pfd10, sizeof(int));
        sendmsg(sp10[0], &mh10, 0);
        int st10 = 0;
        waitpid(c10, &st10, 0);
        close(pfd10);
        close(sp10[0]);
        close(syn10[0]);
        if (WIFEXITED(st10) && WEXITSTATUS(st10) == 0) {
            printf("ENVELOPE_SMOKE: scm-receive-deny PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: scm-receive-deny FAIL st=%d\n", st10);
            failures++;
        }
    }

    {
        /* T11: SCM_RIGHTS send — propagation_types gates authorities
         * leaving the envelope (05 §2.5.3). */
        struct env_policy pb;
        policy_file_rw(&pb); /* propagation_types stays 0 */
        long id11 = env_create(&pb);
        int sp11[2];
        if (id11 < 0 || socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp11) < 0)
            return 1;
        pid_t c11 = fork();
        if (c11 == 0) {
            close(sp11[0]);
            if (env_enter(id11) < 0)
                _exit(101);
            int fd = open("/tmp/env_send.txt",
                          O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (fd < 0)
                _exit(102);
            struct iovec iov = { (void *)"x", 1 };
            union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr al; } u;
            memset(u.buf, 0, sizeof(u.buf));
            struct msghdr mh;
            memset(&mh, 0, sizeof(mh));
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = u.buf;
            mh.msg_controllen = sizeof(u.buf);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &fd, sizeof(int));
            errno = 0;
            ssize_t r = sendmsg(sp11[1], &mh, 0);
            if (!(r < 0 && errno == EPERM))
                _exit(103);
            close(fd);
            _exit(0);
        }
        close(sp11[1]);
        int st11 = 0;
        waitpid(c11, &st11, 0);
        close(sp11[0]);
        if (WIFEXITED(st11) && WEXITSTATUS(st11) == 0) {
            printf("ENVELOPE_SMOKE: scm-send-propagation PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: scm-send-propagation FAIL st=%d\n", st11);
            failures++;
        }
    }

    {
        /* T12: pidfd_getfd is a fresh acquisition against the stealer's
         * envelope (05 §2.5.1 A7): foreign SOCKET class is denied while a
         * FILE descriptor of the same victim is granted. */
        struct env_policy pc;
        policy_file_rw(&pc);
        long id12 = env_create(&pc);
        int syn12[2];
        if (id12 < 0 || pipe(syn12) < 0)
            return 1;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        int pf12 = open("/tmp/env_pidfd.txt", O_CREAT | O_RDWR | O_TRUNC,
                        0644);
        if (sock < 0 || pf12 < 0)
            return 1;
        if (write(pf12, "p", 1) != 1)
            return 1;
        (void)lseek(pf12, 0, SEEK_SET); /* stealer shares the offset */
        pid_t c12 = fork();
        if (c12 == 0) {
            close(syn12[1]);
            if (env_enter(id12) < 0)
                _exit(111);
            int vals[3];
            if (read(syn12[0], vals, sizeof(vals)) != sizeof(vals))
                _exit(112);
            long pidfd = syscall(434 /* SYS_pidfd_open */, vals[0], 0);
            if (pidfd < 0)
                _exit(113);
            errno = 0;
            long rs = syscall(438 /* SYS_pidfd_getfd */, pidfd, vals[1], 0);
            if (!(rs < 0 && errno == EPERM))
                _exit(114);
            errno = 0;
            long rf = syscall(438, pidfd, vals[2], 0);
            if (rf < 0)
                _exit(115);
            char b[1];
            if (read((int)rf, b, 1) != 1)
                _exit(116);
            _exit(0);
        }
        close(syn12[0]);
        int vals12[3] = { getpid(), sock, pf12 };
        if (write(syn12[1], vals12, sizeof(vals12)) != sizeof(vals12))
            return 1;
        int st12 = 0;
        waitpid(c12, &st12, 0);
        close(syn12[1]);
        close(sock);
        close(pf12);
        if (WIFEXITED(st12) && WEXITSTATUS(st12) == 0) {
            printf("ENVELOPE_SMOKE: pidfd-getfd PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: pidfd-getfd FAIL st=%d\n", st12);
            failures++;
        }
    }

    {
        /* T13: attaching a shm segment requires the MEMORY class
         * (05 §2.5.1 A5). */
        struct env_policy pd;
        policy_file_rw(&pd); /* MEMORY absent */
        long id13 = env_create(&pd);
        pid_t c13 = fork();
        if (c13 == 0) {
            if (env_enter(id13) < 0)
                _exit(121);
            int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
            if (shmid < 0) {
                printf("ENVELOPE_SMOKE: shmat-deny SKIP (shmget errno=%d)\n",
                       errno);
                _exit(0);
            }
            void *a = shmat(shmid, NULL, 0);
            int ae = errno;
            shmctl(shmid, IPC_RMID, NULL);
            if (!(a == (void *)-1 && (ae == EPERM || ae == EACCES)))
                _exit(122);
            _exit(0);
        }
        int st13 = 0;
        waitpid(c13, &st13, 0);
        if (WIFEXITED(st13) && WEXITSTATUS(st13) == 0) {
            printf("ENVELOPE_SMOKE: shmat-deny PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: shmat-deny FAIL st=%d\n", st13);
            failures++;
        }
    }

    /* E8 runtime invariant audit across every live envelope (08 §2.4):
     * TypeAllowed / RightsSubCap / budget bounds / attachment consistency
     * must all hold after the whole exercise. */
    {
        unsigned int au[8];
        memset(au, 0, sizeof(au));
        long r = syscall(SYS_a20_envelope_audit, au);
        if (r == 0 && au[3] == 0) {
            printf("ENVELOPE_SMOKE: audit PASS\n");
        } else {
            printf("ENVELOPE_SMOKE: audit FAIL r=%ld viol=%u\n", r,
                   au[3]);
            failures++;
        }
    }

    if (failures == 0) {
        printf("ENVELOPE_SMOKE: PASS\n");
        return 0;
    }
    printf("ENVELOPE_SMOKE: FAIL scenarios=%d\n", failures);
    return 1;
}
