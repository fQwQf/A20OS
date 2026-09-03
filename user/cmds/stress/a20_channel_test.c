/*
 * a20_channel_test — Linux ABI consumer of the unified channel IPC.
 *
 * Exercises the two A20OS Linux-ABI bridge syscalls:
 *   SYS_a20_channel_pair   create a channel pair, use read()/write()
 *   SYS_a20_registry_client  open the well-known service-registry endpoint
 *                            as a file descriptor
 * This is the Linux-side proof that the Native service layer is reachable
 * through ordinary fds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef SYS_a20_channel_pair
#define SYS_a20_channel_pair    900
#endif
#ifndef SYS_a20_registry_client
#define SYS_a20_registry_client 901
#endif
#define SYS_a20_envelope_create 902
#define SYS_a20_envelope_enter  903

struct env_policy {
    unsigned int allowed_types;
    unsigned long long rights_by_class[32];
    unsigned long long time_budget_ns;
    unsigned long long op_budget;
    unsigned long long data_budget;
    unsigned int propagation_types;
    unsigned int flags;
};

int main(void)
{
    int fds[2];
    if (syscall(SYS_a20_channel_pair, fds) != 0) {
        printf("A20_CHANNEL: FAIL channel_pair errno=%d\n", errno);
        return 1;
    }
    if (fds[0] < 0 || fds[1] < 0 || fds[0] == fds[1]) {
        printf("A20_CHANNEL: FAIL bad fds %d %d\n", fds[0], fds[1]);
        return 2;
    }

    const char *msg = "hello-a20-channel";
    ssize_t nw = write(fds[0], msg, strlen(msg));
    if (nw != (ssize_t)strlen(msg)) {
        printf("A20_CHANNEL: FAIL write=%zd errno=%d\n", nw, errno);
        return 3;
    }
    char buf[128];
    ssize_t nr = read(fds[1], buf, sizeof(buf));
    if (nr != (ssize_t)strlen(msg) || memcmp(buf, msg, (size_t)nr) != 0) {
        printf("A20_CHANNEL: FAIL echo got=%zd\n", nr);
        return 4;
    }
    printf("A20_CHANNEL: channel-echo PASS\n");

    int regfd = syscall(SYS_a20_registry_client);
    if (regfd < 0) {
        printf("A20_CHANNEL: FAIL registry_client errno=%d\n", errno);
        return 5;
    }
    if (write(regfd, "ping", 4) != 4) {
        printf("A20_CHANNEL: FAIL registry write errno=%d\n", errno);
        return 6;
    }
    close(regfd);
    close(fds[0]);
    close(fds[1]);

    /* Both Linux bridge calls grant CHANNEL_ENDPOINT authority and must be
     * rejected by a file-only envelope. */
    struct env_policy policy;
    memset(&policy, 0, sizeof(policy));
    policy.allowed_types = 1u << 3; /* A20_OBJ_FILE */
    policy.rights_by_class[3] = (1ull << 0) | (1ull << 1) |
                                (1ull << 3) | (1ull << 4);
    policy.op_budget = 16;
    policy.data_budget = 4096;
    long env_id = syscall(SYS_a20_envelope_create, &policy, 0L);
    if (env_id < 0)
        return 7;
    pid_t child = fork();
    if (child < 0)
        return 8;
    if (child == 0) {
        if (syscall(SYS_a20_envelope_enter, env_id) < 0)
            _exit(9);
        int denied[2];
        errno = 0;
        if (syscall(SYS_a20_channel_pair, denied) != -1 || errno != EPERM)
            _exit(10);
        errno = 0;
        if (syscall(SYS_a20_registry_client) != -1 || errno != EPERM)
            _exit(11);
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("A20_CHANNEL: FAIL envelope mediation status=%d\n", status);
        return 9;
    }
    printf("A20_CHANNEL: envelope-deny PASS\n");

    printf("A20_CHANNEL: PASS\n");
    return 0;
}
