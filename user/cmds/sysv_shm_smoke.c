#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("SYSV_SHM_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int expect_int(const char *what, int got, int expect)
{
    if (got != expect) {
        printf("SYSV_SHM_SMOKE: FAIL %s got=%d expect=%d errno=%d\n",
               what, got, expect, errno);
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
        printf("SYSV_SHM_SMOKE: FAIL %s status=%d\n", what, status);
        return 1;
    }
    return 0;
}

int main(void)
{
    int shmid;
    size_t segsz = 4096;
    struct shmid_ds ds;
    char *addr;

    shmid = shmget(0x45678, segsz, IPC_CREAT | IPC_EXCL | 0666);
    if (shmid < 0)
        return fail("shmget-create");

    if (expect_int("shmget-lookup", shmget(0x45678, 0, 0), shmid) != 0)
        goto fail_rmid;

    addr = shmat(shmid, NULL, 0);
    if (addr == (char *)-1)
        goto fail_rmid_msg;

    memset(addr, 0, segsz);
    addr[0] = 'S';
    addr[1] = 'H';
    addr[2] = 'M';

    if (shmdt(addr) < 0)
        goto fail_rmid_msg;

    addr = shmat(shmid, NULL, 0);
    if (addr == (char *)-1)
        goto fail_rmid_msg;
    if (addr[0] != 'S' || addr[1] != 'H' || addr[2] != 'M') {
        errno = 0;
        shmdt(addr);
        goto fail_rmid;
    }

    if (shmctl(shmid, IPC_STAT, &ds) < 0)
        goto fail_rmid_msg;
    if (ds.shm_segsz < segsz) {
        errno = 0;
        shmdt(addr);
        goto fail_rmid;
    }

    pid_t pid = fork();
    if (pid < 0)
        goto fail_rmid_msg;
    if (pid == 0) {
        char *child_addr = shmat(shmid, NULL, 0);
        if (child_addr == (char *)-1)
            _exit(10);
        if (child_addr[0] != 'S' || child_addr[1] != 'H' || child_addr[2] != 'M') {
            shmdt(child_addr);
            _exit(11);
        }
        child_addr[0] = 'C';
        child_addr[1] = 'H';
        child_addr[2] = 'L';
        if (shmdt(child_addr) < 0)
            _exit(12);
        _exit(0);
    }

    if (wait_child_ok(pid, "wait-child") != 0) {
        shmdt(addr);
        goto fail_rmid;
    }

    if (addr[0] != 'C' || addr[1] != 'H' || addr[2] != 'L') {
        errno = 0;
        shmdt(addr);
        goto fail_rmid;
    }

    if (shmdt(addr) < 0)
        goto fail_rmid_msg;

    if (shmctl(shmid, IPC_RMID, NULL) < 0)
        return fail("shmctl-rmid");

    if (shmat(shmid, NULL, 0) != (char *)-1 || (errno != EINVAL && errno != EIDRM))
        return fail("shmat-after-rmid");

    printf("SYSV_SHM_SMOKE: PASS\n");
    return 0;

fail_rmid_msg:
    fail("syscall");
fail_rmid:
    shmctl(shmid, IPC_RMID, NULL);
    return 1;
}
