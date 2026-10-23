/* SECURITY-01/03: exercise syscall boundaries and retained authority in a
 * real guest. Each irreversible restriction runs in its own child. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "envelope_abi.h"

#define LL_READ 4ULL
#define LL_WRITE 2ULL
static const char *allowed = "/tmp/security-release/allowed";
static const char *outside = "/tmp/security-release-outside";

static int denied_open(const char *path)
{
    errno = 0;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) close(fd);
    return fd == -1 && errno == EACCES;
}

static int wait_code(pid_t child)
{
    int status;
    if (child <= 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) return 127;
    return WEXITSTATUS(status);
}

static int create_abi(void)
{
    uint64_t attr[2] = { LL_READ, 0 };
    if (syscall(SYS_landlock_create_ruleset, attr, 8, 8) != -1 || errno != EINVAL)
        return 1;
    if (syscall(SYS_landlock_create_ruleset, attr, 7, 0) != -1 || errno != EINVAL)
        return 2;
    attr[1] = 1; /* An unimplemented network/scoping field must not be ignored. */
    if (syscall(SYS_landlock_create_ruleset, attr, sizeof(attr), 0) != -1 || errno != E2BIG)
        return 3;
    attr[1] = 0;
    int fd = syscall(SYS_landlock_create_ruleset, attr, sizeof(attr), 0);
    if (fd < 0 || close(fd)) return 4;
    attr[0] = 0;
    if (syscall(SYS_landlock_create_ruleset, attr, 8, 0) != -1 || errno != ENOMSG)
        return 5;
    /* There are no enforcement hooks for these requested restrictions. */
    const uint64_t unsupported[] = { 1, 8, 32, 1ULL << 14, 1ULL << 15 };
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        attr[0] = unsupported[i];
        if (syscall(SYS_landlock_create_ruleset, attr, 8, 0) != -1 ||
            errno != EOPNOTSUPP) return 6 + i;
    }
    return 0;
}

static int add_rule(int rs, const char *path, uint64_t access)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    struct { uint64_t access; int32_t parent; } rule = { access, fd };
    int r = syscall(SYS_landlock_add_rule, rs, 1, &rule, 0);
    close(fd);
    return r;
}

static int restrict_fs(uint64_t handled)
{
    int rs = syscall(SYS_landlock_create_ruleset, &handled, sizeof(handled), 0);
    if (rs < 0) return 1;
    if (add_rule(rs, "/tmp/security-release", handled) || add_rule(rs, "/bin", LL_READ))
        return 2;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
        syscall(SYS_landlock_restrict_self, rs, 0)) return 3;
    return close(rs) ? 4 : 0;
}

static int landlock_permission(void)
{
    if (setgid(65534) || setuid(65534)) return 1;
    uint64_t handled = LL_READ;
    int rs = syscall(SYS_landlock_create_ruleset, &handled, sizeof(handled), 0);
    if (rs < 0 || add_rule(rs, "/tmp/security-release", LL_READ)) return 2;
    if (syscall(SYS_landlock_restrict_self, rs, 0) != -1 || errno != EACCES)
        return 3;
    /* A refused restriction must leave this previously accessible file usable. */
    int fd = open(outside, O_RDONLY);
    if (fd < 0 || close(fd)) return 4;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
        syscall(SYS_landlock_restrict_self, rs, 0) || close(rs)) return 5;
    fd = open(allowed, O_RDONLY);
    if (fd < 0 || close(fd) || !denied_open(outside)) return 6;
    return 0;
}

static int landlock_fork(void)
{
    if (restrict_fs(LL_READ) || !denied_open(outside)) return 1;
    pid_t child = fork();
    if (!child) _exit(denied_open(outside) ? 0 : 2);
    return wait_code(child);
}

static int landlock_exec(void)
{
    if (restrict_fs(LL_READ) || !denied_open(outside)) return 1;
    execl("/bin/security_release_test", "security_release_test", "--landlock-exec", NULL);
    return 2;
}

static int landlock_symlink(void)
{
    if (restrict_fs(LL_READ) || !denied_open(outside)) return 1;
    return denied_open("/tmp/security-release/link") ? 0 : 2;
}

static int landlock_write_aliases(void)
{
    int retained = open(outside, O_RDONLY);
    int dir = open("/tmp/security-release", O_RDONLY | O_DIRECTORY);
    if (retained < 0 || dir < 0 || restrict_fs(LL_READ | LL_WRITE)) return 1;
    int fd = open(allowed, O_WRONLY);
    if (fd < 0 || pwrite(fd, "x", 1, 0) != 1 || close(fd)) return 2;
    fd = open("/tmp/security-release/created", O_CREAT | O_WRONLY, 0600);
    if (fd < 0 || write(fd, "x", 1) != 1 || close(fd)) return 10;
    struct { uint64_t flags, mode, resolve; } create_how = { O_CREAT | O_WRONLY, 0600, 0 };
    fd = syscall(SYS_openat2, dir, "created-at2", &create_how, sizeof(create_how));
    if (fd < 0 || write(fd, "x", 1) != 1 || close(fd)) return 11;
    const char *aliases[] = { "/tmp/security-release/link",
        "/tmp/security-release/../security-release-outside" };
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        fd = open(aliases[i], O_WRONLY | O_TRUNC);
        if (fd >= 0 || errno != EACCES) return 3 + i;
        struct { uint64_t flags, mode, resolve; } how = { O_WRONLY | O_TRUNC, 0, 0 };
        if (syscall(SYS_openat2, AT_FDCWD, aliases[i], &how, sizeof(how)) != -1 ||
            errno != EACCES) return 5 + i;
    }
    if (openat(dir, "escape/security-release-created", O_CREAT | O_WRONLY, 0600) != -1 ||
        errno != EACCES) return 7;
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", retained);
    if (open(path, O_WRONLY | O_TRUNC) != -1 || errno != EACCES) return 8;
    /* Existing descriptors keep their authority; failed reopens must not
     * truncate the underlying object before returning a denial. */
    char data[65], expected[64];
    memset(expected, 'x', sizeof(expected));
    if (pread(retained, data, sizeof(data), 0) != 64 ||
        memcmp(data, expected, 64) || close(retained) || close(dir)) return 9;
    return 0;
}

static int landlock_shared_ruleset(void)
{
    uint64_t handled = LL_READ;
    int rs = syscall(SYS_landlock_create_ruleset, &handled, sizeof(handled), 0);
    int ready[2];
    if (rs < 0 || add_rule(rs, "/tmp/security-release", handled) || pipe(ready)) return 1;
    pid_t child = fork();
    if (!child) {
        close(ready[1]);
        char byte;
        if (read(ready[0], &byte, 1) != 1 || close(ready[0])) _exit(2);
        if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
            syscall(SYS_landlock_restrict_self, rs, 0)) _exit(3);
        if (add_rule(rs, "/tmp/security-release", handled) != -1 || errno != EINVAL)
            _exit(4);
        if (close(rs) || !denied_open(outside)) _exit(5);
        int fd = open(allowed, O_RDONLY);
        _exit(fd >= 0 && !close(fd) ? 0 : 6);
    }
    close(ready[0]);
    int r = prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
        syscall(SYS_landlock_restrict_self, rs, 0) || close(rs);
    if (write(ready[1], "x", 1) != 1 || close(ready[1])) r = 1;
    int result = wait_code(child);
    return r || result || !denied_open(outside) ? 10 + result : 0;
}

static void file_policy(struct env_policy *p)
{
    memset(p, 0, sizeof(*p));
    p->allowed_types = (1u << ENV_OBJ_FILE) | (1u << ENV_OBJ_SOCKET);
    p->rights_by_class[ENV_OBJ_FILE] = ENV_R_READ | ENV_R_WRITE | ENV_R_STAT | ENV_R_SEEK;
    p->rights_by_class[ENV_OBJ_SOCKET] = ENV_R_READ | ENV_R_WRITE | ENV_R_STAT |
        ENV_R_CONNECT | ENV_R_ACCEPT;
    p->propagation_types = 1u << ENV_OBJ_FILE;
    p->op_budget = 1000;
    p->data_budget = 1 << 20;
}

static int transferred_fd(int fd)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv)) return -1;
    char data = 'x';
    struct iovec iov = { &data, 1 };
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    memset(&control, 0, sizeof(control));
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    int got = -1;
    if (sendmsg(sv[0], &msg, 0) == 1) {
        memset(&control, 0, sizeof(control));
        if (recvmsg(sv[1], &msg, 0) == 1 && !(msg.msg_flags & MSG_CTRUNC)) {
            c = CMSG_FIRSTHDR(&msg);
            if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len == CMSG_LEN(sizeof(int)))
                memcpy(&got, CMSG_DATA(c), sizeof(got));
        }
    }
    close(sv[0]); close(sv[1]);
    return got;
}

static int envelope_revoke(void)
{
    struct env_policy p;
    file_policy(&p);
    long id = syscall(SYS_a20_envelope_create, &p, 0L);
    if (id < 0) return 1;
    pid_t child = fork();
    if (!child) {
        if (syscall(SYS_a20_envelope_enter, id)) _exit(2);
        int fds[3];
        fds[0] = open(outside, O_RDWR);
        fds[1] = dup(fds[0]);
        fds[2] = transferred_fd(fds[0]);
        char c;
        for (int i = 0; i < 3; i++)
            if (fds[i] < 0 || pread(fds[i], &c, 1, 0) != 1 || c != 'x') _exit(3);
        /* The parent waits for this stop: every old reference is live before
         * revoke completes, and every denied operation begins after resume. */
        raise(SIGSTOP);
        for (int i = 0; i < 3; i++) {
            c = 'z';
            if (pread(fds[i], &c, 1, 0) != -1 || errno != EACCES || c != 'z' ||
                pwrite(fds[i], "!", 1, 0) != -1 || errno != EACCES || close(fds[i]))
                _exit(4 + i);
        }
        _exit(0);
    }
    int status;
    if (child < 0 || waitpid(child, &status, WUNTRACED) != child) return 7;
    if (!WIFSTOPPED(status)) return WIFEXITED(status) ? 20 + WEXITSTATUS(status) : 8;
    int r = syscall(SYS_a20_envelope_revoke, id);
    kill(child, SIGCONT);
    int result = wait_code(child);
    if (r || result) return result ? result : 9;
    struct {
        uint64_t acquire_ok, deny_type, deny_acquire_rights, use_ok;
        uint64_t deny_ops, deny_data, deny_use_rights, deny_expired, bytes;
        int32_t expired, shadows;
    } stats;
    if (syscall(SYS_a20_envelope_stats, id, &stats) ||
        stats.expired != 1 || stats.shadows != 0) return 11;
    int fd = open(outside, O_RDONLY);
    char c = 0;
    int ok = fd >= 0 && read(fd, &c, 1) == 1 && c == 'x';
    if (fd >= 0) close(fd);
    return ok ? 0 : 10;
}

static int envelope_owner_check(int same_uid)
{
    if (same_uid && (setgid(65534) || setuid(65534))) return 1;
    struct env_policy p;
    file_policy(&p);
    long id = syscall(SYS_a20_envelope_create, &p, 0L);
    if (id < 0) return 2;
    pid_t child = fork();
    if (!child) {
        if (!same_uid && (setgid(65534) || setuid(65534))) _exit(3);
        _exit(syscall(SYS_a20_envelope_revoke, id) == -1 && errno == EPERM ? 0 : 4);
    }
    if (wait_code(child)) return 5;
    if (syscall(SYS_a20_envelope_enter, id)) return 6;
    int fd = open(outside, O_RDONLY);
    char c = 0;
    if (fd < 0 || read(fd, &c, 1) != 1 || c != 'x') return 7;
    /* The denied caller did not revoke it; its owner still can. */
    if (syscall(SYS_a20_envelope_revoke, id)) return 8;
    c = 'z';
    if (pread(fd, &c, 1, 0) != -1 || errno != EACCES || c != 'z' || close(fd))
        return 9;
    return 0;
}

static int envelope_same_uid(void) { return envelope_owner_check(1); }
static int envelope_cross_uid(void) { return envelope_owner_check(0); }

static int envelope_inherited(void)
{
    int fd = open(outside, O_RDONLY);
    char c;
    if (fd < 0 || read(fd, &c, 1) != 1 || c != 'x' || close(fd)) return 1;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock >= 0) close(sock);
    return sock == -1 && errno == EPERM ? 0 : 2;
}

static int envelope_fork_exec(void)
{
    struct env_policy p;
    file_policy(&p);
    p.allowed_types = 1u << ENV_OBJ_FILE;
    long id = syscall(SYS_a20_envelope_create, &p, 0L);
    if (id < 0 || syscall(SYS_a20_envelope_enter, id)) return 1;
    pid_t child = fork();
    if (!child) {
        int result = envelope_inherited();
        if (result) _exit(result);
        execl("/bin/security_release_test", "security_release_test", "--envelope-exec", NULL);
        _exit(3);
    }
    return wait_code(child);
}

static int envelope_quota(int data_budget)
{
    struct env_policy p;
    file_policy(&p);
    if (data_budget) p.data_budget = 1;
    else p.op_budget = 2; /* one acquisition, one use */
    long id = syscall(SYS_a20_envelope_create, &p, 0L);
    if (id < 0) return 1;
    pid_t child = fork();
    if (!child) {
        if (syscall(SYS_a20_envelope_enter, id)) _exit(2);
        int fd = open(outside, O_RDWR);
        char c = 0;
        if (fd < 0 || pread(fd, &c, 1, 0) != 1 || c != 'x') _exit(3);
        if (pwrite(fd, "!", 1, 0) != -1 || errno != EACCES) _exit(4);
        c = 'z';
        if (read(fd, &c, 1) != -1 || errno != EACCES || c != 'z' || close(fd))
            _exit(5);
        _exit(0);
    }
    int result = wait_code(child);
    if (result) return result;
    int fd = open(outside, O_RDONLY);
    char c = 0;
    int ok = fd >= 0 && read(fd, &c, 1) == 1 && c == 'x';
    if (fd >= 0) close(fd);
    return ok ? 0 : 6;
}

static int envelope_op_quota(void) { return envelope_quota(0); }
static int envelope_data_quota(void) { return envelope_quota(1); }

static int probe(const char *name, int (*run)(void))
{
    fflush(stdout);
    pid_t child = fork();
    if (!child) _exit(run());
    int code = wait_code(child);
    printf("SECURITY_RELEASE: %s %s code=%d\n", name, code ? "FAIL" : "PASS", code);
    return code != 0;
}

static int run_program(const char *path, const char *arg)
{
    fflush(stdout);
    pid_t child = fork();
    if (!child) {
        execl(path, path, arg, NULL);
        _exit(127);
    }
    int code = wait_code(child);
    printf("SECURITY_RELEASE: program=%s %s code=%d\n", path,
           code ? "FAIL" : "PASS", code);
    return code != 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Same bytes, syscall counts and kernel build in both arms. The on arm adds
 * an actual deny filter, filesystem restriction and envelope budget; W^X and
 * IOMMU are selected and checked by the guest runner. */
static int benchmark(int enabled)
{
    void *rwx = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (enabled) {
        if (rwx != MAP_FAILED || errno != EACCES) return 1;
        struct filter { uint16_t code; uint8_t jt, jf; uint32_t k; } filter[] = {
            {0x20, 0, 0, 0}, {0x15, 0, 1, SYS_getppid},
            {0x06, 0, 0, 0x00050000U | EPERM}, {0x06, 0, 0, 0x7fff0000U},
        };
        struct { uint16_t count; struct filter *filter; } prog = { 4, filter };
        if (restrict_fs(LL_READ) || prctl(PR_SET_SECCOMP, 2L, &prog, 0L, 0L) ||
            syscall(SYS_getppid) != -1 || errno != EPERM || !denied_open(outside))
            return 2;
        struct env_policy p;
        file_policy(&p);
        p.allowed_types = 1u << ENV_OBJ_FILE;
        p.op_budget = 100000;
        p.data_budget = 8 << 20;
        long id = syscall(SYS_a20_envelope_create, &p, 0L);
        if (id < 0 || syscall(SYS_a20_envelope_enter, id)) return 3;
        if (socket(AF_UNIX, SOCK_STREAM, 0) != -1 || errno != EPERM) return 4;
    } else {
        if (rwx == MAP_FAILED || munmap(rwx, 4096)) return 5;
    }
    int fd = open(allowed, O_RDONLY);
    if (fd < 0) return 6;
    enum { READS = 20000, OPENS = 2000 };
    char bytes[64], expected[64];
    memset(expected, 'x', sizeof(expected));
    for (int sample = 1; sample <= 3; sample++) {
        uint64_t start = now_ns();
        for (int i = 0; i < READS; i++)
            if (pread(fd, bytes, sizeof(bytes), 0) != sizeof(bytes) ||
                memcmp(bytes, expected, sizeof(bytes))) return 7;
        uint64_t read_ns = now_ns() - start;
        start = now_ns();
        for (int i = 0; i < OPENS; i++) {
            int tmp = open(allowed, O_RDONLY);
            if (tmp < 0 || close(tmp)) return 8;
        }
        uint64_t open_ns = now_ns() - start;
        if (!read_ns || !open_ns) return 9;
        printf("SECURITY_PERF: mode=%s sample=%d pread64_ns=%llu open_close_ns=%llu bytes=%u\n",
               enabled ? "on" : "off", sample,
               (unsigned long long)(read_ns / READS),
               (unsigned long long)(open_ns / OPENS), READS * 64);
    }
    if (close(fd)) return 10;
    puts("SECURITY_PERF: PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--landlock-exec"))
        return denied_open(outside) ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "--envelope-exec"))
        return envelope_inherited();
    if (mkdir("/tmp/security-release", 0755) && errno != EEXIST) return 1;
    const char *paths[] = { allowed, outside };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], O_CREAT | O_TRUNC | O_WRONLY, 0644);
        char data[64];
        memset(data, 'x', sizeof(data));
        if (fd < 0 || write(fd, data, sizeof(data)) != sizeof(data) || close(fd)) return 1;
    }
    if (argc == 2 && (!strcmp(argv[1], "--bench-on") || !strcmp(argv[1], "--bench-off"))) {
        int result = benchmark(!strcmp(argv[1], "--bench-on"));
        if (result) printf("SECURITY_PERF: FAIL code=%d errno=%d\n", result, errno);
        /* The unconfined init tears down this guest's tmpfs on exit. */
        return result;
    }
    unlink("/tmp/security-release/link");
    if (symlink(outside, "/tmp/security-release/link")) return 1;
    unlink("/tmp/security-release/escape");
    unlink("/tmp/security-release-created");
    if (symlink("/tmp", "/tmp/security-release/escape")) return 1;
    int failures = probe("landlock-create-abi", create_abi);
    failures += probe("landlock-no-cap", landlock_permission);
    failures += probe("landlock-fork", landlock_fork);
    failures += probe("landlock-exec", landlock_exec);
    failures += probe("landlock-symlink", landlock_symlink);
    failures += probe("landlock-write-aliases-no-effects", landlock_write_aliases);
    failures += probe("landlock-shared-sealed-ruleset", landlock_shared_ruleset);
    struct stat created;
    if (!stat("/tmp/security-release-created", &created) || errno != ENOENT) failures++;
    failures += probe("envelope-revoke-old-refs", envelope_revoke);
    failures += probe("envelope-revoke-non-owner", envelope_same_uid);
    failures += probe("envelope-revoke-cross-uid-no-cap", envelope_cross_uid);
    failures += probe("envelope-fork-exec", envelope_fork_exec);
    failures += probe("envelope-op-quota-no-effects", envelope_op_quota);
    failures += probe("envelope-data-quota-no-effects", envelope_data_quota);
    unlink("/tmp/security-release/link");
    unlink("/tmp/security-release/escape");
    unlink("/tmp/security-release-created");
    unlink("/tmp/security-release/created");
    unlink("/tmp/security-release/created-at2");
    unlink(allowed); unlink(outside); rmdir("/tmp/security-release");
    if (argc == 2 && !strcmp(argv[1], "--candidate")) {
        failures += run_program("/bin/wx_aslr_test", NULL);
        failures += run_program("/bin/sandbox_test", NULL);
        failures += run_program("/bin/cred_state_test", NULL);
        failures += run_program("/bin/native-handle-rv", NULL);
        failures += run_program("/bin/native-mm-rv", "--security-device");
        failures += run_program("/bin/envelope_smoke", NULL);
    }
    printf("SECURITY_RELEASE: %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
