/*
 * fscompat/core/sync.h — 用户态 FS 宿主的空实现互斥体。
 * 语义契约见 core/lock.h 同目录注释；单线程服务进程下全部为 no-op。
 */
#ifndef _SYNC_H
#define _SYNC_H

#include "core/types.h"
#include "core/lock.h"

typedef struct mutex {
    int held;
} mutex_t;

typedef struct rw_mutex {
    int state;
} rw_mutex_t;

static inline void mutex_init(mutex_t *m)
{
    m->held = 0;
}

static inline void mutex_lock(mutex_t *m)
{
    m->held = 1;
}

static inline void mutex_unlock(mutex_t *m)
{
    m->held = 0;
}

static inline int mutex_is_locked(mutex_t *m)
{
    return m->held;
}

static inline void rw_mutex_init(rw_mutex_t *rw)
{
    rw->state = 0;
}

static inline void rw_mutex_read_lock(rw_mutex_t *rw)
{
    rw->state++;
}

static inline void rw_mutex_read_unlock(rw_mutex_t *rw)
{
    rw->state--;
}

static inline void rw_mutex_write_lock(rw_mutex_t *rw)
{
    rw->state = -1;
}

static inline void rw_mutex_write_unlock(rw_mutex_t *rw)
{
    rw->state = 0;
}

#endif /* _SYNC_H */
