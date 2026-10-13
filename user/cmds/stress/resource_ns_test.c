#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "RESOURCE_NS: FAIL line=%d %s errno=%d\n",         \
                    __LINE__, #x, errno);                                      \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static int unshare_ns(unsigned flags) {
    return syscall(SYS_unshare, flags);
}
static int join_ns(int fd, int kind) {
    return syscall(SYS_setns, fd, kind);
}
static int wait_ok(pid_t pid) {
    int st;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && !WEXITSTATUS(st);
}
static unsigned long long ns_ino(const char *kind) {
    char path[80], buf[128] = {0};
    snprintf(path, sizeof(path), "/proc/self/ns/%s", kind);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unsigned long long ino = 0;
    if (n > 0) {
        char *p = strchr(buf, '[');
        if (p)
            ino = strtoull(p + 1, NULL, 10);
    }
    return ino;
}
static int uts_test(void) {
    struct utsname before, after;
    CHECK(uname(&before) == 0);
    unsigned long long old = ns_ino("uts");
    CHECK(old);
    int fd = open("/proc/self/ns/uts", O_RDONLY);
    CHECK(fd >= 0);
    CHECK(unshare_ns(CLONE_NEWUTS) == 0);
    CHECK(ns_ino("uts") != old);
    CHECK(uname(&after) == 0 && !strcmp(after.nodename, before.nodename));
    CHECK(sethostname("isolated", 8) == 0 && setdomainname("private", 7) == 0);
    CHECK(uname(&after) == 0 && !strcmp(after.nodename, "isolated") &&
          !strcmp(after.domainname, "private"));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid)
        _exit(uname(&after) < 0 || strcmp(after.nodename, "isolated"));
    CHECK(wait_ok(pid));
    CHECK(join_ns(fd, CLONE_NEWIPC) == -1 && errno == EINVAL);
    CHECK(join_ns(fd, 0) == 0 && ns_ino("uts") == old);
    CHECK(uname(&after) == 0 && !strcmp(after.nodename, before.nodename));
    close(fd);
    /* A user namespace's capabilities do not grant control of host UTS. */
    pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        if (unshare_ns(CLONE_NEWUSER) < 0)
            _exit(1);
        errno = 0;
        if (sethostname("escape", 6) != -1 || errno != EPERM)
            _exit(2);
        _exit(0);
    }
    CHECK(wait_ok(pid));
    puts("RESOURCE_NS: uts PASS copy+isolation+inherit+setns+owner");
    return 0;
}
static int ipc_test(void) {
    int pm = msgget(IPC_PRIVATE, 0600);
    int ps = semget(IPC_PRIVATE, 1, 0600);
    int ph = shmget(IPC_PRIVATE, 4096, 0600);
    CHECK(pm >= 0 && ps >= 0 && ph >= 0);
    CHECK(msgctl(pm, IPC_RMID, NULL) == 0 && semctl(ps, 0, IPC_RMID) == 0 &&
          shmctl(ph, IPC_RMID, NULL) == 0);
    int key = 0x5a20000 + (getpid() & 0xffff);
    int msg = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(msg >= 0);
    int sem = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(sem >= 0);
    int shm = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(shm >= 0);
    char mqname[64];
    snprintf(mqname, sizeof(mqname), "/ns-test-%d", getpid());
    struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = 64};
    mqd_t mq = mq_open(mqname, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    CHECK(mq != (mqd_t)-1);
    CHECK(mq_send(mq, "host", 5, 0) == 0);
    int fd = open("/proc/self/ns/ipc", O_RDONLY);
    CHECK(fd >= 0);
    char *mapping = shmat(shm, NULL, 0);
    CHECK(mapping != (void *)-1);
    strcpy(mapping, "mapped-host");
    unsigned long long old = ns_ino("ipc");
    CHECK(unshare_ns(CLONE_NEWIPC) == 0 && ns_ino("ipc") != old);
    struct msqid_ds md;
    struct shmid_ds sd;
    CHECK(msgctl(msg, IPC_STAT, &md) == -1 && errno == EINVAL);
    CHECK(semctl(sem, 0, GETVAL) == -1 && errno == EINVAL);
    CHECK(shmctl(shm, IPC_STAT, &sd) == -1 && errno == EINVAL);
    CHECK(!strcmp(mapping, "mapped-host"));
    int m2 = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(m2 >= 0 && m2 != msg);
    int s2 = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(s2 >= 0 && s2 != sem);
    int h2 = shmget(key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(h2 >= 0 && h2 != shm);
    mqd_t q2 = mq_open(mqname, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
    CHECK(q2 != (mqd_t)-1);
    char data[64];
    CHECK(mq_receive(mq, data, sizeof(data), NULL) == 5 &&
          !strcmp(data, "host"));
    CHECK(mq_send(q2, "private", 8, 0) == 0);
    /* The namespace handle pins all resources after its last task leaves. */
    int private_fd = open("/proc/self/ns/ipc", O_RDONLY);
    CHECK(private_fd >= 0);
    unsigned long long private_ino = ns_ino("ipc");
    CHECK(join_ns(fd, CLONE_NEWIPC) == 0 && ns_ino("ipc") == old);
    CHECK(msgget(key, 0) == msg && semget(key, 0, 0) == sem &&
          shmget(key, 0, 0) == shm);
    CHECK(join_ns(private_fd, 0) == 0 && ns_ino("ipc") == private_ino);
    CHECK(msgget(key, 0) == m2 && semget(key, 0, 0) == s2 &&
          shmget(key, 0, 0) == h2);
    CHECK(mq_receive(q2, data, sizeof(data), NULL) == 8 &&
          !strcmp(data, "private"));
    CHECK(join_ns(fd, 0) == 0);
    close(private_fd); /* destroys private SysV objects, unlinks private MQ */
    CHECK(mq_close(q2) == 0);
    CHECK(msgctl(msg, IPC_RMID, NULL) == 0 && semctl(sem, 0, IPC_RMID) == 0 &&
          shmctl(shm, IPC_RMID, NULL) == 0);
    CHECK(shmdt(mapping) == 0);
    CHECK(mq_unlink(mqname) == 0 && mq_close(mq) == 0);
    close(fd);
    puts("RESOURCE_NS: ipc PASS keys+ids+shm-mapping+mq+setns+lifetime");
    return 0;
}
static int udp_socket(unsigned port) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (void *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
static int net_test(void) {
    const unsigned port = 24000 + (getpid() % 1000);
    int host = udp_socket(port);
    CHECK(host >= 0);
    int oldfd = open("/proc/self/ns/net", O_RDONLY);
    CHECK(oldfd >= 0);
    unsigned long long old = ns_ino("net");
    CHECK(unshare_ns(CLONE_NEWNET) == 0 && ns_ino("net") != old);
    int local = udp_socket(port);
    CHECK(local >= 0); /* independent port space */
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(tx >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_port = htons(port),
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(sendto(tx, "private", 8, 0, (void *)&addr, sizeof(addr)) == -1 &&
          errno == ENETUNREACH);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    CHECK(ioctl(tx, SIOCGIFFLAGS, &ifr) == 0 && !(ifr.ifr_flags & IFF_UP));
    ifr.ifr_flags |= IFF_UP;
    CHECK(ioctl(tx, SIOCSIFFLAGS, &ifr) == 0);
    CHECK(sendto(tx, "private", 8, 0, (void *)&addr, sizeof(addr)) == 8);
    char data[64];
    CHECK(recv(local, data, sizeof(data), 0) == 8 && !strcmp(data, "private"));
    CHECK(recv(host, data, sizeof(data), 0) == -1 && errno == EAGAIN);
    addr.sin_addr.s_addr = inet_addr("10.0.2.2");
    CHECK(sendto(tx, "escape", 7, 0, (void *)&addr, sizeof(addr)) == -1 &&
          errno == ENETUNREACH);
    strcpy(ifr.ifr_name, "eth0");
    CHECK(ioctl(tx, SIOCGIFINDEX, &ifr) == -1 && errno == ENODEV);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        _exit(sendto(tx, "child", 6, 0, (void *)&addr, sizeof(addr)) != 6);
    }
    CHECK(wait_ok(pid));
    CHECK(recv(local, data, sizeof(data), 0) == 6 && !strcmp(data, "child"));
    /* TCP uses the same isolated port space, with both address families. */
    for (int family = AF_INET; family <= AF_INET6;
         family += AF_INET6 - AF_INET) {
        int listener = socket(family, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        struct sockaddr_in6 a6 = {.sin6_family = AF_INET6,
                                  .sin6_port = htons(port)};
        a6.sin6_addr = in6addr_loopback;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        void *target = family == AF_INET ? (void *)&addr : (void *)&a6;
        socklen_t size = family == AF_INET ? sizeof(addr) : sizeof(a6);
        CHECK(bind(listener, target, size) == 0 && listen(listener, 4) == 0);
        int client = socket(family, SOCK_STREAM, 0);
        CHECK(client >= 0);
        CHECK(connect(client, target, size) == 0);
        int accepted = accept(listener, NULL, NULL);
        CHECK(accepted >= 0);
        CHECK(write(client, "tcp", 4) == 4 &&
              read(accepted, data, sizeof(data)) == 4 && !strcmp(data, "tcp"));
        close(client);
        close(accepted);
        close(listener);
    }
    mkdir("/tmp/ns-sys", 0755);
    CHECK(mount("none", "/tmp/ns-sys", "sysfs", 0, NULL) == 0);
    CHECK(access("/tmp/ns-sys/class/net/lo/ifindex", F_OK) == 0);
    CHECK(access("/tmp/ns-sys/class/net/net0", F_OK) == -1 && errno == ENOENT);
    CHECK(umount("/tmp/ns-sys") == 0);
    CHECK(join_ns(oldfd, 0) == 0 && ns_ino("net") == old);
    /* An existing socket stays attached to its original namespace. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(sendto(tx, "pinned", 7, 0, (void *)&addr, sizeof(addr)) == 7);
    CHECK(recv(local, data, sizeof(data), 0) == 7 && !strcmp(data, "pinned"));
    CHECK(recv(host, data, sizeof(data), 0) == -1 && errno == EAGAIN);
    close(tx);
    close(local);
    close(host);
    close(oldfd);
    puts("RESOURCE_NS: net PASS "
         "ports+loopback+no-host-route+inherit+socket-pin");
    return 0;
}
static int cgroup_test(void) {
    mkdir("/tmp/ns-cg", 0755);
    mkdir("/tmp/ns-cg-view", 0755);
    CHECK(mount("none", "/tmp/ns-cg", "cgroup2", 0, NULL) == 0);
    CHECK(mkdir("/tmp/ns-cg/private", 0755) == 0);
    int fd = open("/tmp/ns-cg/private/cgroup.procs", O_WRONLY);
    CHECK(fd >= 0);
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%d", getpid());
    CHECK(write(fd, pid, n) == n);
    close(fd);
    unsigned long long old = ns_ino("cgroup");
    CHECK(unshare_ns(CLONE_NEWCGROUP) == 0 && ns_ino("cgroup") != old);
    fd = open("/proc/self/cgroup", O_RDONLY);
    CHECK(fd >= 0);
    char path[128] = {0};
    CHECK(read(fd, path, sizeof(path) - 1) > 0);
    close(fd);
    CHECK(!strcmp(path, "0::/\n"));
    CHECK(mount("none", "/tmp/ns-cg-view", "cgroup2", 0, NULL) == 0);
    CHECK(access("/tmp/ns-cg-view/cgroup.procs", F_OK) == 0);
    CHECK(access("/tmp/ns-cg-view/private", F_OK) == -1 && errno == ENOENT);
    CHECK(rmdir("/tmp/ns-cg/private") == -1 && errno == EBUSY);
    puts("RESOURCE_NS: cgroup PASS relative-proc+mount-root+pin");
    return 0;
}
static int clone_test(void) {
    const char *kinds[] = {"uts", "ipc", "net", "cgroup"};
    unsigned long long before[4];
    for (int i = 0; i < 4; i++) {
        before[i] = ns_ino(kinds[i]);
        CHECK(before[i]);
    }
    pid_t pid = syscall(SYS_clone,
                        CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET |
                            CLONE_NEWCGROUP | SIGCHLD,
                        0, 0, 0, 0);
    CHECK(pid >= 0);
    if (!pid) {
        for (int i = 0; i < 4; i++)
            if (ns_ino(kinds[i]) == before[i])
                _exit(1);
        _exit(0);
    }
    CHECK(wait_ok(pid));
    for (int i = 0; i < 4; i++)
        CHECK(ns_ino(kinds[i]) == before[i]);
    /* Repeated last-reference destruction must reclaim the bounded IPC tables. */
    for (int i = 0; i < 40; i++) {
        CHECK(unshare_ns(CLONE_NEWIPC) == 0);
        CHECK(msgget(0x5a20, IPC_CREAT | IPC_EXCL | 0600) >= 0);
        CHECK(semget(0x5a20, 1, IPC_CREAT | IPC_EXCL | 0600) >= 0);
        CHECK(shmget(0x5a20, 4096, IPC_CREAT | IPC_EXCL | 0600) >= 0);
    }
    puts("RESOURCE_NS: clone+lifetime PASS");
    return 0;
}
static int unprivileged_test(void) {
    CHECK(setgid(65534) == 0 && setuid(65534) == 0);
    CHECK(unshare_ns(CLONE_NEWNET) == -1 && errno == EPERM);
    CHECK(unshare_ns(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWUTS |
                     CLONE_NEWIPC | CLONE_NEWCGROUP) == 0);
    CHECK(sethostname("owned", 5) == 0);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(fd >= 0);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    ifr.ifr_flags = IFF_UP;
    CHECK(ioctl(fd, SIOCSIFFLAGS, &ifr) == 0);
    close(fd);
    puts("RESOURCE_NS: unprivileged PASS scoped-owner");
    return 0;
}
int main(void) {
    int (*tests[])(void) = {uts_test,    ipc_test,   net_test,
                            cgroup_test, clone_test, unprivileged_test};
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (!pid)
            _exit(tests[i]());
        CHECK(wait_ok(pid));
    }
    puts("RESOURCE_NS: PASS");
    return 0;
}
