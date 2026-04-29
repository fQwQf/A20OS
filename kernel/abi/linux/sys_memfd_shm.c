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
    (void)shmflg;
    size_t size;
    int r = sysv_shm_size(shmid, &size);
    if (r < 0) return r;
    return sys_mmap((uint64_t)shmaddr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

int64_t sys_shmdt(const void *shmaddr)
{
    return sysv_shm_detach(shmaddr);
}

int64_t sys_shmctl(int shmid, int cmd, void *buf)
{
    (void)buf;
    return sysv_shm_control(shmid, cmd);
}
