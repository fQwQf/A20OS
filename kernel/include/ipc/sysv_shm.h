#ifndef _IPC_SYSV_SHM_H
#define _IPC_SYSV_SHM_H

#include "core/types.h"

int sysv_shm_get(int key, size_t size, int shmflg);
int sysv_shm_size(int shmid, size_t *size);
int sysv_shm_detach(const void *shmaddr);
int sysv_shm_control(int shmid, int cmd);
uint64_t sysv_shm_at(int shmid, uint64_t shmaddr, int shmflg);

#endif /* _IPC_SYSV_SHM_H */
