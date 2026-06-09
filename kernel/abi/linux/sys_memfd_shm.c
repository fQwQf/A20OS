#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "fs/memfd.h"
#include "ipc/sysv_shm.h"
#include "ipc/sysv_sem.h"
#include "core/timer.h"

struct linux_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

int64_t sys_memfd_create(const char *name, unsigned flags)
{
    (void)name;
    return memfd_create_file((int)flags);
}

int64_t sys_shmget(int key, size_t size, int shmflg)
{
    return sysv_shm_get(key, size, shmflg);
}

int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg)
{
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
