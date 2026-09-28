/*
 * fscompat/core/sync.h — no-op mutexes for the user-space FS host.  See the
 * comments in core/lock.h in the same directory for the semantic contract;
 * under a single-threaded service process everything is a no-op.
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
