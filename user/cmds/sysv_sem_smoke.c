#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

union semun_local {
    int val;
    unsigned short *array;
    void *buf;
};

static int fail(const char *what)
{
    printf("SYSV_SEM_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int expect_int(const char *what, int got, int expect)
{
    if (got != expect) {
        printf("SYSV_SEM_SMOKE: FAIL %s got=%d expect=%d errno=%d\n", what, got, expect, errno);
        return 1;
    }
    return 0;
}

static int wait_child_ok(pid_t pid, const char *what)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail(what);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("SYSV_SEM_SMOKE: FAIL %s status=%d\n", what, status);
        return 1;
    }
    return 0;
}

int main(void)
{
    int semid;
    union semun_local arg;
    unsigned short vals[2] = {1, 2};
    unsigned short out[2] = {0, 0};
    struct sembuf sop;

    semid = semget(0x23456, 2, IPC_CREAT | IPC_EXCL | 0600);
    if (semid < 0)
        return fail("semget-create");

    if (expect_int("semget-lookup", semget(0x23456, 1, 0), semid) != 0)
        goto fail_rmid;

    arg.array = vals;
    if (semctl(semid, 0, SETALL, arg) < 0)
        goto fail_rmid_msg;

    if (expect_int("getval-0", semctl(semid, 0, GETVAL), 1) != 0)
        goto fail_rmid;
    if (expect_int("getval-1", semctl(semid, 1, GETVAL), 2) != 0)
        goto fail_rmid;

    arg.array = out;
    if (semctl(semid, 0, GETALL, arg) < 0)
        goto fail_rmid_msg;
    if (out[0] != 1 || out[1] != 2) {
        errno = 0;
        return fail("getall-values");
    }

    arg.val = 3;
    if (semctl(semid, 0, SETVAL, arg) < 0)
        goto fail_rmid_msg;
    if (expect_int("setval-getval", semctl(semid, 0, GETVAL), 3) != 0)
        goto fail_rmid;
    if (semctl(semid, 0, GETPID) <= 0) {
        errno = 0;
        return fail("getpid");
    }
    if (expect_int("getncnt", semctl(semid, 0, GETNCNT), 0) != 0)
        goto fail_rmid;
    if (expect_int("getzcnt", semctl(semid, 0, GETZCNT), 0) != 0)
        goto fail_rmid;
    if (semctl(semid, 0, IPC_STAT, (void *)out) < 0)
        goto fail_rmid_msg;
    if (expect_int("sem-stat", semctl(semid, 0, SEM_STAT, (void *)out), semid) != 0)
        goto fail_rmid;
    if (expect_int("sem-info", semctl(semid, 0, SEM_INFO, (void *)out), 32) != 0)
        goto fail_rmid;

    sop.sem_num = 1;
    sop.sem_op = -2;
    sop.sem_flg = 0;
    if (semop(semid, &sop, 1) < 0)
        goto fail_rmid_msg;
    if (expect_int("after-semop", semctl(semid, 1, GETVAL), 0) != 0)
        goto fail_rmid;

    arg.val = 0;
    if (semctl(semid, 0, SETVAL, arg) < 0)
        goto fail_rmid_msg;

    pid_t pid = fork();
    if (pid < 0)
        goto fail_rmid_msg;
    if (pid == 0) {
        struct sembuf child_op;
        child_op.sem_num = 0;
        child_op.sem_op = -1;
        child_op.sem_flg = 0;
        if (semop(semid, &child_op, 1) < 0)
            _exit(10);
        _exit(0);
    }

    usleep(20000);
    arg.val = 4;
    if (semctl(semid, 0, SETVAL, arg) < 0)
        goto fail_rmid_msg;
    if (wait_child_ok(pid, "wait-blocked-child") != 0)
        goto fail_rmid;
    if (expect_int("after-wake", semctl(semid, 0, GETVAL), 3) != 0)
        goto fail_rmid;

    {
        struct timespec ts;
        memset(&ts, 0, sizeof(ts));
        ts.tv_nsec = 50 * 1000 * 1000;
        sop.sem_num = 1;
        sop.sem_op = -1;
        sop.sem_flg = 0;
        errno = 0;
        if (semtimedop(semid, &sop, 1, &ts) != -1 || errno != EAGAIN) {
            printf("SYSV_SEM_SMOKE: FAIL semtimedop-timeout ret=%d errno=%d\n", 0, errno);
            goto fail_rmid;
        }
    }

    if (semctl(semid, 0, IPC_RMID) < 0)
        return fail("semctl-rmid");
    if (semop(semid, &sop, 1) != -1 || errno != EINVAL)
        return fail("semop-after-rmid");

    printf("SYSV_SEM_SMOKE: PASS\n");
    return 0;

fail_rmid_msg:
    fail("syscall");
fail_rmid:
    semctl(semid, 0, IPC_RMID);
    return 1;
}
