#include "ipc/sysv_shm.h"

#include "core/consts.h"
#include "core/defs.h"

#define SYSV_SHM_MAX 32

typedef struct {
    int used;
    int key;
    size_t size;
} sysv_shm_t;

static sysv_shm_t g_shm[SYSV_SHM_MAX];

int sysv_shm_get(int key, size_t size, int shmflg)
{
    (void)shmflg;
    if (size == 0) return -EINVAL;
    for (int i = 0; i < SYSV_SHM_MAX; i++)
        if (g_shm[i].used && g_shm[i].key == key) return i;
    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!g_shm[i].used) {
            g_shm[i].used = 1;
            g_shm[i].key = key;
            g_shm[i].size = ROUND_UP(size, PAGE_SIZE);
            return i;
        }
    }
    return -ENOSPC;
}

int sysv_shm_size(int shmid, size_t *size)
{
    if (!size) return -EINVAL;
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) return -EINVAL;
    *size = g_shm[shmid].size;
    return 0;
}

int sysv_shm_detach(const void *shmaddr)
{
    if (!shmaddr) return -EINVAL;
    return 0;
}

int sysv_shm_control(int shmid, int cmd)
{
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) return -EINVAL;
    if (cmd == 0) {
        g_shm[shmid].used = 0;
        return 0;
    }
    return 0;
}
