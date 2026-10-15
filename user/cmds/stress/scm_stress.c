#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <poll.h>
#include <stddef.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include "../core/envelope_abi.h"

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
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#define SCM_ENV_OBJ_TASK        1
#define SCM_ENV_OBJ_TIMER       9
#define SCM_ENV_OBJ_EVENT_QUEUE 8
#define SCM_ENV_OBJ_SOCKET      5
#define SCM_ENV_OBJ_FILE        3
#define SCM_SOURCE_SIGNALFD     32

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

static long scm_test_envelope_create(unsigned propagation_types)
{
    struct env_policy policy;
    memset(&policy, 0, sizeof(policy));
    policy.allowed_types = (1u << SCM_ENV_OBJ_FILE) |
                           (1u << SCM_ENV_OBJ_SOCKET) |
                           (1u << SCM_ENV_OBJ_EVENT_QUEUE) |
                           (1u << SCM_ENV_OBJ_TIMER) |
                           (1u << SCM_ENV_OBJ_TASK);
    for (size_t i = 0; i < 32; ++i)
        policy.rights_by_class[i] = ~0ull;
    policy.op_budget = 1000000;
    policy.data_budget = 1u << 20;
    policy.propagation_types = propagation_types;
    return syscall(SYS_a20_envelope_create, &policy, 0L);
}

static int scm_test_object_fd(int kind)
{
    switch (kind) {
    case SCM_ENV_OBJ_FILE:
        return open("/scm_envelope_source", O_CREAT | O_TRUNC | O_RDWR, 0600);
    case SCM_ENV_OBJ_SOCKET:
        return socket(AF_UNIX, SOCK_DGRAM, 0);
    case SCM_ENV_OBJ_EVENT_QUEUE:
        return eventfd(0, 0);
    case SCM_SOURCE_SIGNALFD: {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        return signalfd(-1, &mask, SFD_NONBLOCK);
    }
    case SCM_ENV_OBJ_TIMER:
        return timerfd_create(CLOCK_MONOTONIC, 0);
    case SCM_ENV_OBJ_TASK: {
        long fd = syscall(SYS_pidfd_open, (int)getpid(), 0);
        return fd > INT32_MAX ? -1 : (int)fd;
    }
    default:
        return -1;
    }
}

static int scm_envelope_class_case(int kind, unsigned propagation, int allow)
{
    long env = scm_test_envelope_create(propagation);
    if (env < 0)
        return fail("envelope class create");
    pid_t child = fork();
    if (child < 0)
        return fail("envelope class fork");
    if (child == 0) {
        if (syscall(SYS_a20_envelope_enter, env) < 0)
            _exit(10);
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) < 0)
            _exit(11);
        int source = scm_test_object_fd(kind);
        if (source < 0)
            _exit(12);
        char payload = 'v';
        errno = 0;
        int sent = send_fds(pair[0], &source, 1, &payload, 1, 0);
        if (!allow) {
            if (sent != -1 || errno != EPERM)
                _exit(13);
        } else {
            int received = -1;
            char byte = 0;
            if (sent != 1 || recv_fds(pair[1], &received, 1, &byte, 1,
                                     0, NULL) != 1 || byte != payload)
                _exit(14);
            close(received);
        }
        close(source);
        close(pair[0]);
        close(pair[1]);
        _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("SCM_STRESS: envelope class=%d allow=%d child-status=%d\n",
               kind, allow, status);
        return 1;
    }
    return 0;
}

typedef struct scm_aba_race {
    int target_fd;
    int file_fd;
    int socket_fd;
    int transport[2];
    volatile unsigned ready;
    volatile int go;
    volatile int stop;
    volatile int active;
    volatile int sender_primed;
    volatile int replacement_started;
    volatile int leak;
    volatile int unexpected;
    volatile unsigned attempts;
    volatile unsigned active_attempts;
    volatile unsigned post_replacement_attempts;
    volatile unsigned replacements;
    volatile unsigned sent_socket;
    volatile unsigned denied_file;
    volatile unsigned identity_mismatch;
} scm_aba_race_t;

#define SCM_ABA_RACE_ITERATIONS 20000

static void *scm_aba_replacer(void *opaque)
{
    scm_aba_race_t *race = opaque;
    __atomic_fetch_add(&race->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&race->go, __ATOMIC_ACQUIRE))
        sched_yield();
    __atomic_store_n(&race->active, 1, __ATOMIC_RELEASE);
    /* The sender pauses after priming the active phase. Replace the target
     * once before releasing it, proving subsequent send attempts overlap the
     * close/reuse churn rather than merely preceding it. */
    while (!__atomic_load_n(&race->sender_primed, __ATOMIC_ACQUIRE))
        sched_yield();
    if (dup2(race->socket_fd, race->target_fd) < 0) {
        __atomic_store_n(&race->unexpected, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&race->active, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&race->stop, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_fetch_add(&race->replacements, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&race->replacement_started, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < SCM_ABA_RACE_ITERATIONS; ++i) {
        if (dup2(race->socket_fd, race->target_fd) < 0) {
            __atomic_store_n(&race->unexpected, 1, __ATOMIC_RELEASE);
            break;
        }
        __atomic_fetch_add(&race->replacements, 1, __ATOMIC_RELAXED);
        sched_yield();
        if (dup2(race->file_fd, race->target_fd) < 0) {
            __atomic_store_n(&race->unexpected, 1, __ATOMIC_RELEASE);
            break;
        }
        __atomic_fetch_add(&race->replacements, 1, __ATOMIC_RELAXED);
        sched_yield();
    }
    __atomic_store_n(&race->active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&race->stop, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *scm_aba_sender(void *opaque)
{
    scm_aba_race_t *race = opaque;
    __atomic_fetch_add(&race->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&race->go, __ATOMIC_ACQUIRE))
        sched_yield();
    unsigned attempts = 0;
    while (attempts++ < SCM_ABA_RACE_ITERATIONS) {
        char payload = 'a';
        if (__atomic_load_n(&race->active, __ATOMIC_ACQUIRE))
            __atomic_fetch_add(&race->active_attempts, 1, __ATOMIC_RELAXED);
        if (attempts == 32) {
            __atomic_store_n(&race->sender_primed, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(&race->replacement_started,
                                    __ATOMIC_ACQUIRE))
                sched_yield();
        }
        if (__atomic_load_n(&race->replacement_started, __ATOMIC_ACQUIRE))
            __atomic_fetch_add(&race->post_replacement_attempts, 1,
                               __ATOMIC_RELAXED);
        int sent = send_fds(race->transport[0], &race->target_fd, 1,
                            &payload, 1, 0);
        __atomic_fetch_add(&race->attempts, 1, __ATOMIC_RELAXED);
        if (sent == 1) {
            int received = -1;
            char byte = 0;
            if (recv_fds(race->transport[1], &received, 1, &byte, 1,
                         0, NULL) != 1 || byte != payload) {
                __atomic_store_n(&race->unexpected, 1, __ATOMIC_RELEASE);
                break;
            }
            int socket_type = 0;
            socklen_t socket_type_len = sizeof(socket_type);
            if (getsockopt(received, SOL_SOCKET, SO_TYPE, &socket_type,
                           &socket_type_len) < 0 ||
                socket_type != SOCK_DGRAM)
                __atomic_store_n(&race->leak, 1, __ATOMIC_RELEASE);
            close(received);
            __atomic_fetch_add(&race->sent_socket, 1, __ATOMIC_RELAXED);
        } else if (errno == EPERM) {
            __atomic_fetch_add(&race->denied_file, 1, __ATOMIC_RELAXED);
        } else if (errno == EBADF) {
            __atomic_fetch_add(&race->identity_mismatch, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_store_n(&race->unexpected, 1, __ATOMIC_RELEASE);
            break;
        }
        if ((attempts & 7u) == 0)
            sched_yield();
        if (__atomic_load_n(&race->stop, __ATOMIC_ACQUIRE) &&
            attempts > 1000)
            break;
    }
    return NULL;
}

static int scm_envelope_close_reuse_race(void)
{
    long env = scm_test_envelope_create(1u << SCM_ENV_OBJ_SOCKET);
    if (env < 0)
        return fail("envelope ABA create");
    pid_t child = fork();
    if (child < 0)
        return fail("envelope ABA fork");
    if (child == 0) {
        if (syscall(SYS_a20_envelope_enter, env) < 0)
            _exit(20);
        int transport[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, transport) < 0)
            _exit(21);
        int file = open("/scm_aba_secret", O_CREAT | O_TRUNC | O_RDWR, 0600);
        int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        int target = 64;
        if (file < 0 || socket_fd < 0 || dup2(file, target) != target)
            _exit(22);
        scm_aba_race_t race = {
            .target_fd = target,
            .file_fd = file,
            .socket_fd = socket_fd,
            .transport = {transport[0], transport[1]},
            .ready = 0,
            .go = 0,
            .stop = 0,
            .active = 0,
            .sender_primed = 0,
            .replacement_started = 0,
            .leak = 0,
            .unexpected = 0,
            .attempts = 0,
            .active_attempts = 0,
            .post_replacement_attempts = 0,
            .replacements = 0,
            .sent_socket = 0,
            .denied_file = 0,
            .identity_mismatch = 0,
        };
        /* Establish both policy outcomes before introducing close/reuse
         * contention, so scheduler timing cannot erase either assertion. */
        char baseline_payload = 'b';
        errno = 0;
        if (send_fds(transport[0], &target, 1, &baseline_payload, 1, 0) != -1 ||
            errno != EPERM)
            _exit(25);
        if (dup2(socket_fd, target) != target ||
            send_fds(transport[0], &target, 1, &baseline_payload, 1, 0) != 1)
            _exit(26);
        int baseline_received = -1;
        char baseline_byte = 0;
        if (recv_fds(transport[1], &baseline_received, 1, &baseline_byte, 1,
                     0, NULL) != 1 || baseline_byte != baseline_payload)
            _exit(27);
        int baseline_type = 0;
        socklen_t baseline_type_len = sizeof(baseline_type);
        if (getsockopt(baseline_received, SOL_SOCKET, SO_TYPE, &baseline_type,
                       &baseline_type_len) < 0 || baseline_type != SOCK_DGRAM)
            _exit(28);
        close(baseline_received);
        if (dup2(file, target) != target)
            _exit(29);
        pthread_t replacer, sender;
        if (pthread_create(&replacer, NULL, scm_aba_replacer, &race) ||
            pthread_create(&sender, NULL, scm_aba_sender, &race))
            _exit(23);
        while (__atomic_load_n(&race.ready, __ATOMIC_ACQUIRE) != 2)
            sched_yield();
        __atomic_store_n(&race.go, 1, __ATOMIC_RELEASE);
        pthread_join(replacer, NULL);
        pthread_join(sender, NULL);
        if (__atomic_load_n(&race.leak, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&race.unexpected, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&race.attempts, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&race.active_attempts, __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&race.post_replacement_attempts,
                            __ATOMIC_ACQUIRE) == 0 ||
            __atomic_load_n(&race.replacements, __ATOMIC_ACQUIRE) == 0) {
            printf("SCM_STRESS: envelope close-reuse details leak=%d unexpected=%d "
                   "attempts=%u active=%u post-replacement=%u replacements=%u socket=%u "
                   "file-deny=%u identity-mismatch=%u\n",
                   __atomic_load_n(&race.leak, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.unexpected, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.attempts, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.active_attempts, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.post_replacement_attempts,
                                   __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.replacements, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.sent_socket, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.denied_file, __ATOMIC_ACQUIRE),
                   __atomic_load_n(&race.identity_mismatch, __ATOMIC_ACQUIRE));
            fflush(stdout);
            _exit(24);
        }
        close(target);
        close(file);
        close(socket_fd);
        close(transport[0]);
        close(transport[1]);
        _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("SCM_STRESS: envelope close-reuse status=%d\n", status);
        return 1;
    }
    puts("SCM_STRESS: envelope-pinned-close-reuse PASS");
    return 0;
}

static int scm_envelope_class_matrix(void)
{
    static const struct {
        int source;
        int class;
    } cases[] = {
        {SCM_ENV_OBJ_FILE, SCM_ENV_OBJ_FILE},
        {SCM_ENV_OBJ_SOCKET, SCM_ENV_OBJ_SOCKET},
        {SCM_ENV_OBJ_EVENT_QUEUE, SCM_ENV_OBJ_EVENT_QUEUE},
        {SCM_SOURCE_SIGNALFD, SCM_ENV_OBJ_EVENT_QUEUE},
        {SCM_ENV_OBJ_TIMER, SCM_ENV_OBJ_TIMER},
        {SCM_ENV_OBJ_TASK, SCM_ENV_OBJ_TASK},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (scm_envelope_class_case(cases[i].source,
                                    1u << cases[i].class, 1) != 0 ||
            scm_envelope_class_case(cases[i].source, 0, 0) != 0)
            return 1;
    }
    puts("SCM_STRESS: envelope-vfile-classes PASS");
    return scm_envelope_close_reuse_race();
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

    if (scm_envelope_class_matrix() != 0)
        return 1;

    close(rfd);
    close(f);
    unlink("/scm_stress_tmp");
    close(sv[0]);
    close(sv[1]);

    printf("SCM_STRESS: PASS\n");
    return 0;
}
