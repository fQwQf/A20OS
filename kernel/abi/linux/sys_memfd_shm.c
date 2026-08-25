#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "fs/memfd.h"
#include "ipc/sysv_shm.h"
#include "ipc/sysv_sem.h"
#include "ipc/sysv_msg.h"
#include "core/timer.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

struct linux_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

int64_t sys_memfd_create(const char *name, unsigned flags)
{
    (void)name;
    int ufd = memfd_create_file((int)flags);
    if (ufd < 0)
        return ufd;
    int gfd = fdtable_get_current(ufd);
    env_kind_register(gfd, A20_OBJ_FILE);
    if (env_active(proc_current())) {
        uint64_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                          A20_RIGHT_STAT | A20_RIGHT_SEEK;
        int mr = env_mediate_acquire((uint8_t)A20_OBJ_FILE, rights, gfd);
        if (mr) {
            fdtable_close_current(ufd);
            return mr;
        }
    }
    return ufd;
}

int64_t sys_shmget(int key, size_t size, int shmflg)
{
    return sysv_shm_get(key, size, shmflg);
}

int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg)
{
    /* A5 (docs/research/05 §2.5.4): attach is a MEMORY-class acquisition;
     * footprint accounting lands with the shmat rework in W2. */
    if (env_active(proc_current())) {
        int mr = env_mediate_class((uint8_t)A20_OBJ_MEMORY);
        if (mr)
            return mr;
    }
    return (int64_t)sysv_shm_at(shmid, (uint64_t)(uintptr_t)shmaddr, shmflg);
}

int64_t sys_shmdt(const void *shmaddr)
{
    return sysv_shm_detach(shmaddr);
}

int64_t sys_shmctl(int shmid, int cmd, void *buf)
{
    return sysv_shm_control(shmid, cmd, buf);
}

int64_t sys_semget(int key, int nsems, int semflg)
{
    return sysv_sem_get(key, nsems, semflg);
}

int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg)
{
    void *uarg = (void *)(uintptr_t)arg;
    if ((cmd & ~0x100) == 16)
        uarg = (void *)(uintptr_t)(arg & 0xffff);
    return sysv_sem_control(semid, semnum, cmd, uarg);
}

int64_t sys_semtimedop(int semid, const void *sops, size_t nsops, const void *timeout)
{
    uint64_t deadline = 0;
    if (timeout) {
        struct linux_timespec64 ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return -EINVAL;
        uint64_t ticks = (uint64_t)ts.tv_sec * TICKS_PER_SEC +
                         (uint64_t)ts.tv_nsec * TICKS_PER_SEC / 1000000000ULL;
        if ((ts.tv_sec || ts.tv_nsec) && ticks == 0)
            ticks = 1;
        deadline = timer_get_ticks() + ticks;
    }
    return sysv_sem_timedop(semid, sops, nsops, deadline);
}

int64_t sys_semop(int semid, const void *sops, size_t nsops)
{
    return sysv_sem_op(semid, sops, nsops);
}

int64_t sys_msgget(int key, int msgflg)
{
    return sysv_msg_get(key, msgflg);
}

int64_t sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    return sysv_msg_send(msqid, msgp, msgsz, msgflg);
}

int64_t sys_msgrcv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp,
                   int msgflg)
{
    return sysv_msg_recv(msqid, msgp, msgsz, msgtyp, msgflg);
}

int64_t sys_msgctl(int msqid, int cmd, void *buf)
{
    return sysv_msg_control(msqid, cmd, buf);
}
