/* Linux sandbox ABI: sticky no_new_privs and pointed-to classic BPF filters. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* Linux UAPI wire layouts, independent of host kernel headers. */
struct sock_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};
struct sock_fprog {
    uint16_t len;
    struct sock_filter *filter;
};
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_ALLOW 0x7fff0000U

static int wait_ok(pid_t pid) {
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

static int filter_probe(void) {
    struct sock_filter insns[] = {
        {0x20, 0, 0, 0},
        {0x15, 0, 1, SYS_getppid},
        {0x06, 0, 0, SECCOMP_RET_ERRNO | EPERM},
        {0x06, 0, 0, SECCOMP_RET_ALLOW},
    };
    struct sock_fprog prog = {.len = 4, .filter = insns};
    if (prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) != 0)
        return 1;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0L, 0L))
        return 2;
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != EPERM ||
        syscall(SYS_getpid) <= 0)
        return 3;
    pid_t child = fork();
    if (!child) {
        errno = 0;
        int denied = syscall(SYS_getppid) == -1 && errno == EPERM;
        _exit(denied && prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == 1 ? 0
                                                                        : 4);
    }
    return wait_ok(child) ? 0 : 5;
}

static int filter_stack_probe(void) {
    /* LDX/STX scratch memory must preserve A; a newer TRACE must not
     * weaken an older ERRNO, and equally ranked newest ERRNO data wins. */
    struct sock_filter insns[] = {
        {0x20, 0, 0, 0},
        {0x01, 0, 0, SYS_getppid},
        {0x03, 0, 0, 0},
        {0x61, 0, 0, 0},
        {0x1d, 0, 1, 0},
        {0x06, 0, 0, SECCOMP_RET_ERRNO | EPERM},
        {0x06, 0, 0, SECCOMP_RET_ALLOW},
    };
    struct sock_fprog prog = {.len = 7, .filter = insns};
    if (prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) != 0 ||
        prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) ||
        syscall(SYS_seccomp, 1, 0, &prog))
        return 1;
    if (prctl(PR_GET_SECCOMP, 0L, 0L, 0L, 0L) != 2)
        return 2;
    insns[5].k = 0x7ff00000U;
    if (syscall(SYS_seccomp, 1, 0, &prog))
        return 3;
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != EPERM)
        return 4;
    insns[5].k = SECCOMP_RET_ERRNO | EACCES;
    if (syscall(SYS_seccomp, 1, 0, &prog))
        return 5;
    errno = 0;
    if (syscall(SYS_getppid) != -1 || errno != EACCES)
        return 6;
    return syscall(SYS_getpid) > 0 ? 0 : 7;
}

static int exec_probe(int enabled) {
    if (setgid(65534) || setuid(65534))
        return 1;
    if (enabled && prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L))
        return 2;
    execl("/tmp/sandbox-setid", "sandbox_test", enabled ? "--nnp" : "--setid",
          NULL);
    return 3;
}

int main(int argc, char **argv) {
    if (argc == 2) {
        int enabled = strcmp(argv[1], "--nnp") == 0;
        return geteuid() == (enabled ? 65534 : 0) &&
                       prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == enabled
                   ? 0
                   : 1;
    }
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) != -1 || errno != EINVAL)
        goto fail;
    errno = 0;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1L, 1L, 0L, 0L) != -1 || errno != EINVAL)
        goto fail;
    pid_t child = fork();
    if (!child)
        _exit(filter_probe());
    if (!wait_ok(child))
        goto fail;
    child = fork();
    if (!child)
        _exit(filter_stack_probe());
    if (!wait_ok(child))
        goto fail;
    uint32_t action = SECCOMP_RET_ALLOW;
    if (syscall(SYS_seccomp, 2, 0, &action) || action != SECCOMP_RET_ALLOW)
        goto fail;
    action = 0x12340000U;
    errno = 0;
    if (syscall(SYS_seccomp, 2, 0, &action) != -1 || errno != EOPNOTSUPP ||
        action != 0x12340000U)
        goto fail;
    int in = open("/proc/self/exe", O_RDONLY);
    int out = open("/tmp/sandbox-setid", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        goto fail;
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n)
            goto fail;
    }
    close(in);
    close(out);
    if (n < 0 || chmod("/tmp/sandbox-setid", 04755))
        goto fail;
    for (int enabled = 0; enabled < 2; enabled++) {
        child = fork();
        if (!child)
            _exit(exec_probe(enabled));
        if (!wait_ok(child))
            goto fail;
    }
    unlink("/tmp/sandbox-setid");
    puts("SANDBOX: PASS no-new-privs fork+exec+setid "
         "seccomp-pointer+errno+inherit+stack+scratch+mode");
    return 0;
fail:
    unlink("/tmp/sandbox-setid");
    printf("SANDBOX: FAIL errno=%d\n", errno);
    return 1;
}
