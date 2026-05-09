#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "fs/memfd.h"
#include "ipc/sysv_shm.h"

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
