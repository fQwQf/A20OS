#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/*
 * Guest-side acceptance for the credential transition policy.
 *
 * The program is deliberately written against the Linux ABI only: it is the
 * same binary on the CRED_IMPL=c and CRED_IMPL=rust kernels, and the checker
 * compares both serial logs.  Every branch that the two implementations could
 * disagree on prints the observed values, so a divergence shows up as a
 * different line rather than as a bare FAIL.
 *
 * Coverage: the set*uid/set*gid saved-id matrix and its EPERM refusals, the
 * setfsuid/setfsgid previous-value convention, the capset(2) EPERM matrix
 * including the CAP_SETPCAP inheritable path, and credential isolation between
 * a parent and a child forked on either side of the transition.
 */

#ifndef SYS_capget
#define SYS_capget 90
#endif
#ifndef SYS_capset
#define SYS_capset 91
#endif

#define LINUX_CAPABILITY_VERSION_3 0x20080522U
#define CAP_SETPCAP 8

struct cap_hdr {
    uint32_t version;
    int pid;
};

struct cap_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

static int fail(const char *what, long value) {
    printf("CRED_STATE_TEST: FAIL %s value=%ld errno=%d\n", what, value, errno);
    return 1;
}

static long cap_get(uint64_t *eff, uint64_t *perm, uint64_t *inh) {
    struct cap_hdr hdr = {LINUX_CAPABILITY_VERSION_3, 0};
    struct cap_data data[2];
    memset(data, 0, sizeof(data));
    long rc = syscall(SYS_capget, &hdr, data);
    if (rc < 0)
        return rc;
    *eff = (uint64_t)data[0].effective | ((uint64_t)data[1].effective << 32);
    *perm = (uint64_t)data[0].permitted | ((uint64_t)data[1].permitted << 32);
    *inh =
        (uint64_t)data[0].inheritable | ((uint64_t)data[1].inheritable << 32);
    return 0;
}

static long cap_set(uint64_t eff, uint64_t perm, uint64_t inh) {
    struct cap_hdr hdr = {LINUX_CAPABILITY_VERSION_3, 0};
    struct cap_data data[2];
    data[0].effective = (uint32_t)eff;
    data[0].permitted = (uint32_t)perm;
    data[0].inheritable = (uint32_t)inh;
    data[1].effective = (uint32_t)(eff >> 32);
    data[1].permitted = (uint32_t)(perm >> 32);
    data[1].inheritable = (uint32_t)(inh >> 32);
    return syscall(SYS_capset, &hdr, data);
}

/* Runs with the inherited full capability set.  Reports the first failing
 * check index over the pipe so the parent keeps a single deterministic log. */
static void capset_child(int fd) {
    int step = 0;
    uint64_t eff = 0, perm = 0, inh = 0;

    /* Identity write keeps the full sets. */
    step = 1;
    if (cap_set(~(uint64_t)0, ~(uint64_t)0, 0) != 0)
        goto done;
    step = 2;
    /* CAP_SETPCAP is effective, so the bounding set admits any inheritable bit. */
    if (cap_set(~(uint64_t)0, ~(uint64_t)0, 1ULL << 9) != 0)
        goto done;
    step = 3;
    /* Effective may shrink without touching permitted. */
    if (cap_set(0, ~(uint64_t)0, 1ULL << 9) != 0)
        goto done;
    step = 4;
    /* With permitted full, any inheritable growth is still admitted. */
    if (cap_set(0, ~(uint64_t)0, (1ULL << 9) | (1ULL << 20)) != 0)
        goto done;
    step = 5;
    /* Shrink permitted to CAP_SETUID only; effective keeps it. */
    if (cap_set(1ULL << 8, 1ULL << 8, 0) != 0)
        goto done;
    step = 6;
    /* Effective holds CAP_SETPCAP, so the bounding set admits bit 9. */
    if (cap_set(1ULL << 8, 1ULL << 8, 1ULL << 9) != 0)
        goto done;
    step = 7;
    /* Reset inheritable, then drop CAP_SETPCAP from effective. */
    if (cap_set(0, 1ULL << 8, 0) != 0)
        goto done;
    step = 8;
    /* No CAP_SETPCAP effective and bit 9 outside inheritable|permitted. */
    if (cap_set(0, 1ULL << 8, 1ULL << 9) != -1 || errno != EPERM)
        goto done;
    step = 9;
    /* Permitted may not grow. */
    if (cap_set(0, (1ULL << 8) | (1ULL << 9), 0) != -1 || errno != EPERM)
        goto done;
    step = 10;
    /* Effective must stay inside permitted. */
    if (cap_set(1ULL << 9, 1ULL << 8, 0) != -1 || errno != EPERM)
        goto done;
    step = 11;
    if (cap_get(&eff, &perm, &inh) != 0)
        goto done;
    if (eff != 0 || perm != (1ULL << 8) || inh != 0)
        goto done;
    step = 12;
    /* A refused capset must not have applied part of its request. */
    if (cap_set(0, 1ULL << 8, 0) != 0)
        goto done;
    step = 0;

done:
    {
        int rc = step;
        ssize_t ignored = write(fd, &rc, sizeof(rc));
        (void)ignored;
    }
    _exit(0);
}

static void id_child(int fd) {
    /* The child inherits the parent's post-transition identity; its own
     * setresuid must not reach the parent. */
    int report[4];
    int rc = setresuid(-1, 1000, -1);
    report[0] = rc == 0 ? 0 : -errno;
    uid_t ruid = 0, euid = 0, suid = 0;
    report[1] = getresuid(&ruid, &euid, &suid) == 0 ? (int)euid : -errno;
    report[2] = (int)ruid;
    report[3] = (int)suid;
    ssize_t ignored = write(fd, report, sizeof(report));
    (void)ignored;
    _exit(0);
}

int main(void) {
    printf("CRED_STATE_TEST: start\n");

    uid_t ruid = 0, euid = 0, suid = 0;
    gid_t rgid = 0, egid = 0, sgid = 0;
    uint64_t eff = 0, perm = 0, inh = 0;

    /* The initial process runs with the full capability set, which the capset
     * matrix needs.  Exercise it in a child so the parent keeps it. */
    {
        int pipefd[2];
        if (pipe(pipefd) != 0)
            return fail("capset-pipe", 0);
        pid_t pid = fork();
        if (pid < 0)
            return fail("capset-fork", 0);
        if (pid == 0) {
            close(pipefd[0]);
            capset_child(pipefd[1]);
        }
        close(pipefd[1]);
        int step = -1;
        if (read(pipefd[0], &step, sizeof(step)) != (ssize_t)sizeof(step))
            return fail("capset-report", 0);
        close(pipefd[0]);
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("capset-wait", 0);
        if (step != 0)
            return fail("capset-matrix", step);
        printf("CRED_STATE_TEST: capset-matrix PASS\n");
    }

    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-initial", 0);
    if (ruid || euid || suid)
        return fail("initial-uid", (long)ruid);
    if (getresgid(&rgid, &egid, &sgid) != 0)
        return fail("getresgid-initial", 0);
    if (rgid || egid || sgid)
        return fail("initial-gid", (long)rgid);
    if (cap_get(&eff, &perm, &inh) != 0)
        return fail("capget-initial", 0);
    if (eff != ~(uint64_t)0 || perm != ~(uint64_t)0 || inh != 0)
        return fail("initial-caps", (long)perm);
    printf("CRED_STATE_TEST: initial-identity PASS\n");

    /* Root dropping to three distinct ids must clear effective and permitted. */
    if (setresuid(1000, 1001, 1002) != 0)
        return fail("setresuid-root", 0);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-drop", 0);
    if (ruid != 1000 || euid != 1001 || suid != 1002)
        return fail("setresuid-root-values", (long)euid);
    if (cap_get(&eff, &perm, &inh) != 0)
        return fail("capget-after-drop", 0);
    if (eff != 0 || perm != 0)
        return fail("root-drop-clears-caps", (long)perm);
    printf("CRED_STATE_TEST: root-drop PASS ids=%d,%d,%d\n", (int)ruid,
           (int)euid, (int)suid);

    /* A foreign id is refused and nothing moves. */
    errno = 0;
    if (setuid(2000) != -1 || errno != EPERM)
        return fail("setuid-foreign", errno);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-refusal", 0);
    if (ruid != 1000 || euid != 1001 || suid != 1002)
        return fail("setuid-foreign-unchanged", (long)euid);

    /* The saved id is a current id, so it may become the effective id. */
    if (setuid(1002) != 0)
        return fail("setuid-saved-id", errno);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-saved", 0);
    if (ruid != 1000 || euid != 1002 || suid != 1002)
        return fail("setuid-saved-values", (long)euid);

    /* An all -1 request succeeds and changes nothing. */
    if (setresuid(-1, -1, -1) != 0)
        return fail("setresuid-noop", errno);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-noop", 0);
    if (ruid != 1000 || euid != 1002 || suid != 1002)
        return fail("setresuid-noop-unchanged", (long)ruid);

    /* setresuid may not reach an id that is not already current. */
    errno = 0;
    if (setresuid(-1, 0, -1) != -1 || errno != EPERM)
        return fail("setresuid-foreign", errno);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-resrefusal", 0);
    if (ruid != 1000 || euid != 1002 || suid != 1002)
        return fail("setresuid-foreign-unchanged", (long)euid);

    /* setreuid re-saves the new effective id. */
    if (setreuid(1002, 1000) != 0)
        return fail("setreuid-current", errno);
    if (getresuid(&ruid, &euid, &suid) != 0)
        return fail("getresuid-after-setreuid", 0);
    if (ruid != 1002 || euid != 1000 || suid != 1000)
        return fail("setreuid-values", (long)suid);
    printf("CRED_STATE_TEST: uid-matrix PASS ids=%d,%d,%d\n", (int)ruid,
           (int)euid, (int)suid);

    /* setfsuid reports the previous value and ignores a foreign id. */
    if (syscall(SYS_setfsuid, 1002) != 1000)
        return fail("setfsuid-previous", errno);
    if (syscall(SYS_setfsuid, 4242) != 1002)
        return fail("setfsuid-foreign-previous", errno);
    if (syscall(SYS_setfsuid, 1002) != 1002)
        return fail("setfsuid-foreign-unchanged", errno);
    printf("CRED_STATE_TEST: setfsuid PASS\n");

    /* Group family: the inherited egid is still 0, so it stays reachable. */
    errno = 0;
    if (setgid(2000) != -1 || errno != EPERM)
        return fail("setgid-foreign", errno);
    if (setgid(0) != 0)
        return fail("setgid-current", errno);
    if (getresgid(&rgid, &egid, &sgid) != 0)
        return fail("getresgid-after-setgid", 0);
    if (rgid != 0 || egid != 0 || sgid != 0)
        return fail("setgid-values", (long)egid);

    if (setregid(0, 0) != 0)
        return fail("setregid-current", errno);
    if (setresgid(-1, 0, 0) != 0)
        return fail("setresgid-current", errno);
    errno = 0;
    if (setregid(-1, 2000) != -1 || errno != EPERM)
        return fail("setregid-foreign", errno);
    if (getresgid(&rgid, &egid, &sgid) != 0)
        return fail("getresgid-final", 0);
    if (rgid != 0 || egid != 0 || sgid != 0)
        return fail("group-matrix-values", (long)sgid);
    if (syscall(SYS_setfsgid, 0) != 0)
        return fail("setfsgid-previous", errno);
    printf("CRED_STATE_TEST: group-matrix PASS\n");

    /* A child forked after the transition inherits it, and its own setresuid
     * must leave the parent's identity alone. */
    {
        int pipefd[2];
        if (pipe(pipefd) != 0)
            return fail("id-pipe", 0);
        pid_t pid = fork();
        if (pid < 0)
            return fail("id-fork", 0);
        if (pid == 0) {
            close(pipefd[0]);
            id_child(pipefd[1]);
        }
        close(pipefd[1]);
        int report[4];
        if (read(pipefd[0], report, sizeof(report)) != (ssize_t)sizeof(report))
            return fail("id-report", 0);
        close(pipefd[0]);
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("id-wait", 0);
        if (report[0] != 0)
            return fail("child-setresuid", report[0]);
        if (report[1] != 1000)
            return fail("child-euid", report[1]);
        if (getresuid(&ruid, &euid, &suid) != 0)
            return fail("getresuid-after-child", 0);
        if (ruid != 1002 || euid != 1000 || suid != 1000)
            return fail("parent-id-unchanged", (long)euid);
    }
    printf("CRED_STATE_TEST: fork-isolation PASS\n");

    /* Concurrent workers: independent tasks walk their own saved-id ladder in
     * parallel, so the transition must stay confined to the calling task. */
    {
        enum { CRED_WORKERS = 4, CRED_ROUNDS = 200 };
        pid_t workers[CRED_WORKERS];
        for (int i = 0; i < CRED_WORKERS; i++) {
            workers[i] = fork();
            if (workers[i] < 0)
                return fail("worker-fork", i);
            if (workers[i] == 0) {
                int bad = 0;
                uid_t w_ruid = 0, w_euid = 0, w_suid = 0;
                if (getresuid(&w_ruid, &w_euid, &w_suid) != 0)
                    _exit(1);
                int cur_fsuid = (int)w_euid;
                for (int round = 0; round < CRED_ROUNDS; round++) {
                    if (setresuid(-1, (uid_t)w_euid, -1) != 0) {
                        bad = 2;
                        break;
                    }
                    if (syscall(SYS_setfsuid, (long)(int)w_ruid) != cur_fsuid) {
                        bad = 3;
                        break;
                    }
                    cur_fsuid = (int)w_ruid;
                    if (syscall(SYS_setfsuid, (long)(int)w_euid) != cur_fsuid) {
                        bad = 4;
                        break;
                    }
                    cur_fsuid = (int)w_euid;
                    uid_t now_ruid = 0, now_euid = 0, now_suid = 0;
                    if (getresuid(&now_ruid, &now_euid, &now_suid) != 0 ||
                        now_ruid != w_ruid || now_euid != w_euid ||
                        now_suid != w_suid) {
                        bad = 5;
                        break;
                    }
                }
                _exit(bad);
            }
        }
        for (int i = 0; i < CRED_WORKERS; i++) {
            int status = 0;
            if (waitpid(workers[i], &status, 0) != workers[i])
                return fail("worker-wait", i);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return fail("worker-status", (long)WEXITSTATUS(status));
        }
        if (getresuid(&ruid, &euid, &suid) != 0)
            return fail("getresuid-after-workers", 0);
        if (ruid != 1002 || euid != 1000 || suid != 1000)
            return fail("parent-id-after-workers", (long)euid);
    }
    printf("CRED_STATE_TEST: concurrent-workers PASS\n");

    printf("CRED_STATE_TEST: PASS\n");
    return 0;
}
