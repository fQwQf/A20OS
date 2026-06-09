#ifndef _IPC_SYSV_SEM_H
#define _IPC_SYSV_SEM_H

#include "core/types.h"

int sysv_sem_get(int key, int nsems, int semflg);
int sysv_sem_control(int semid, int semnum, int cmd, void *arg);
int sysv_sem_op(int semid, const void *sops, size_t nsops);
int sysv_sem_timedop(int semid, const void *sops, size_t nsops, uint64_t deadline);

#endif /* _IPC_SYSV_SEM_H */
