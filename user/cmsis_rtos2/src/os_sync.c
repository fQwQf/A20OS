/*
 * CMSIS-RTOS2 mutexes, semaphores and event flags.
 *
 * All three objects are one lock-protected counter plus a blocking queue.
 * Ownership rules and recursion live here so backends only provide a timed
 * wait, never object semantics.
 */
#include "os_impl.h"


/* ------------------------------------------------------------------------- */
/* Mutex                                                                     */
/* ------------------------------------------------------------------------- */

static int mutex_valid(osMutexId_t m)
{
    return m && m->obj.magic == OS_MAGIC && m->obj.kind == OS_KIND_MUTEX;
}

osMutexId_t osMutexNew(const osMutexAttr_t *attr)
{
    if (os_core_init() != 0)
        return NULL;

#if !OS_CFG_MUTEX
    (void)attr;
    return NULL;
#else
    struct os_mutex_cb *m;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*m))
            return NULL;
        m = (struct os_mutex_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        m = (struct os_mutex_cb *)osb_alloc(sizeof(*m));
        if (!m)
            return NULL;
    }
    os_memzero(m, sizeof(*m));
    m->obj.magic = OS_MAGIC;
    m->obj.kind = OS_KIND_MUTEX;
    m->obj.static_cb = (uint16_t)static_cb;
    m->obj.q = osb_bq_create();
    if (!m->obj.q) {
        if (!static_cb)
            osb_free(m);
        return NULL;
    }
    os_name_store(m->obj.name, (attr && attr->name) ? attr->name : "mutex");
    m->attr = attr ? attr->attr_bits : (osMutexRecursive | osMutexPrioInherit);
    if (!(m->attr & osMutexRecursive))
        m->attr &= ~(uint32_t)osMutexRecursive;
    return m;
#endif
}

const char *osMutexGetName(osMutexId_t mutex_id)
{
    return mutex_valid(mutex_id) ? mutex_id->obj.name : NULL;
}

static osStatus_t mutex_acquire(osMutexId_t m, uint32_t timeout)
{
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(m->obj.q);
    for (;;) {
        if (!m->owner) {
            m->owner = self;
            m->count = 1;
            osb_bq_unlock(m->obj.q);
            self->held_mutexes++;
            return osOK;
        }
        if (m->owner == self) {
            if (m->attr & osMutexRecursive) {
                m->count++;
                osb_bq_unlock(m->obj.q);
                return osOK;
            }
            osb_bq_unlock(m->obj.q);
            return osErrorResource;
        }
        if (timeout == 0) {
            osb_bq_unlock(m->obj.q);
            return osErrorResource;
        }
        /*
         * Priority inheritance: the owner runs at our priority until it
         * releases the mutex, so a medium-priority thread cannot keep it
         * from making progress.  The backend keeps the maximum donation and
         * the core drops it when the owner's last mutex is released.
         */
        if (m->attr & osMutexPrioInherit)
            (void)osb_thread_boost(m->owner);
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(m->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(m->obj.q);
            return osErrorTimeout;
        }
    }
}

osStatus_t osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout)
{
    if (!mutex_valid(mutex_id))
        return osErrorParameter;
    return mutex_acquire(mutex_id, timeout);
}

osStatus_t osMutexRelease(osMutexId_t mutex_id)
{
    if (!mutex_valid(mutex_id))
        return osErrorParameter;
    osThreadId_t self = os_current_thread();
    int final = 0;

    osb_bq_lock(mutex_id->obj.q);
    if (mutex_id->owner != self) {
        osb_bq_unlock(mutex_id->obj.q);
        return osErrorResource;
    }
    if (--mutex_id->count == 0) {
        mutex_id->owner = NULL;
        /*
         * Wake every waiter: the scheduler then runs the highest-priority
         * one first, which is the CMSIS handoff order.  A single wake would
         * hand the mutex to the oldest waiter regardless of priority.
         */
        osb_bq_wake_all(mutex_id->obj.q);
        final = 1;
    }
    osb_bq_unlock(mutex_id->obj.q);

    if (final) {
        /* Drop the inheritance donation only when no mutex is held any
         * more; a lower-priority mutex may still be contended. */
        if (self->held_mutexes > 0)
            self->held_mutexes--;
        if (self->held_mutexes == 0)
            osb_thread_unboost(self);
    }
    return osOK;
}

osThreadId_t osMutexGetOwner(osMutexId_t mutex_id)
{
    if (!mutex_valid(mutex_id))
        return NULL;
    osb_bq_lock(mutex_id->obj.q);
    osThreadId_t owner = mutex_id->owner;
    osb_bq_unlock(mutex_id->obj.q);
    return owner;
}

osStatus_t osMutexDelete(osMutexId_t mutex_id)
{
    if (!mutex_valid(mutex_id))
        return osErrorParameter;
    osb_bq_lock(mutex_id->obj.q);
    if (mutex_id->owner) {
        osb_bq_unlock(mutex_id->obj.q);
        return osErrorResource;
    }
    osb_bq_unlock(mutex_id->obj.q);

    osb_bq_destroy(mutex_id->obj.q);
    mutex_id->obj.magic = 0;
    if (!mutex_id->obj.static_cb)
        osb_free(mutex_id);
    return osOK;
}

/* ------------------------------------------------------------------------- */
/* Semaphore                                                                 */
/* ------------------------------------------------------------------------- */

static int sem_valid(osSemaphoreId_t s)
{
    return s && s->obj.magic == OS_MAGIC && s->obj.kind == OS_KIND_SEM;
}

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                               const osSemaphoreAttr_t *attr)
{
    if (os_core_init() != 0)
        return NULL;
#if !OS_CFG_SEMAPHORE
    (void)max_count; (void)initial_count; (void)attr;
    return NULL;
#else
    if (max_count == 0 || initial_count > max_count)
        return NULL;

    struct os_semaphore_cb *s;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*s))
            return NULL;
        s = (struct os_semaphore_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        s = (struct os_semaphore_cb *)osb_alloc(sizeof(*s));
        if (!s)
            return NULL;
    }
    os_memzero(s, sizeof(*s));
    s->obj.magic = OS_MAGIC;
    s->obj.kind = OS_KIND_SEM;
    s->obj.static_cb = (uint16_t)static_cb;
    s->obj.q = osb_bq_create();
    if (!s->obj.q) {
        if (!static_cb)
            osb_free(s);
        return NULL;
    }
    os_name_store(s->obj.name, (attr && attr->name) ? attr->name : "sem");
    s->count = initial_count;
    s->max_count = max_count;
    return s;
#endif
}

const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id)
{
    return sem_valid(semaphore_id) ? semaphore_id->obj.name : NULL;
}

osStatus_t osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout)
{
    if (!sem_valid(semaphore_id))
        return osErrorParameter;
#if !OS_CFG_SEMAPHORE
    return osErrorResource;
#else
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(semaphore_id->obj.q);
    for (;;) {
        if (semaphore_id->count > 0) {
            semaphore_id->count--;
            osb_bq_unlock(semaphore_id->obj.q);
            return osOK;
        }
        if (timeout == 0) {
            osb_bq_unlock(semaphore_id->obj.q);
            return osErrorResource;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(semaphore_id->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(semaphore_id->obj.q);
            return osErrorTimeout;
        }
    }
#endif
}

osStatus_t osSemaphoreRelease(osSemaphoreId_t semaphore_id)
{
    if (!sem_valid(semaphore_id))
        return osErrorParameter;
#if !OS_CFG_SEMAPHORE
    return osErrorResource;
#else
    osb_bq_lock(semaphore_id->obj.q);
    if (semaphore_id->count >= semaphore_id->max_count) {
        osb_bq_unlock(semaphore_id->obj.q);
        return osErrorResource;
    }
    semaphore_id->count++;
    /*
     * Broadcast, not a single handoff: the woken set is ordered by the
     * scheduler's priority queues, so the highest-priority waiter takes the
     * token.  Lower-priority waiters re-check the count and sleep again.
     */
    osb_bq_wake_all(semaphore_id->obj.q);
    osb_bq_unlock(semaphore_id->obj.q);
    return osOK;
#endif
}

uint32_t osSemaphoreGetCount(osSemaphoreId_t semaphore_id)
{
    if (!sem_valid(semaphore_id))
        return 0;
    osb_bq_lock(semaphore_id->obj.q);
    uint32_t n = semaphore_id->count;
    osb_bq_unlock(semaphore_id->obj.q);
    return n;
}

osStatus_t osSemaphoreDelete(osSemaphoreId_t semaphore_id)
{
    if (!sem_valid(semaphore_id))
        return osErrorParameter;
    osb_bq_destroy(semaphore_id->obj.q);
    semaphore_id->obj.magic = 0;
    if (!semaphore_id->obj.static_cb)
        osb_free(semaphore_id);
    return osOK;
}

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */
/* ------------------------------------------------------------------------- */

static int ef_valid(osEventFlagsId_t e)
{
    return e && e->obj.magic == OS_MAGIC && e->obj.kind == OS_KIND_EF;
}

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr)
{
    if (os_core_init() != 0)
        return NULL;
#if !OS_CFG_EVENT_FLAGS
    (void)attr;
    return NULL;
#else
    struct os_eventflags_cb *e;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*e))
            return NULL;
        e = (struct os_eventflags_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        e = (struct os_eventflags_cb *)osb_alloc(sizeof(*e));
        if (!e)
            return NULL;
    }
    os_memzero(e, sizeof(*e));
    e->obj.magic = OS_MAGIC;
    e->obj.kind = OS_KIND_EF;
    e->obj.static_cb = (uint16_t)static_cb;
    e->obj.q = osb_bq_create();
    if (!e->obj.q) {
        if (!static_cb)
            osb_free(e);
        return NULL;
    }
    os_name_store(e->obj.name, (attr && attr->name) ? attr->name : "ef");
    return e;
#endif
}

const char *osEventFlagsGetName(osEventFlagsId_t ef_id)
{
    return ef_valid(ef_id) ? ef_id->obj.name : NULL;
}

uint32_t osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags)
{
    if (!ef_valid(ef_id))
        return osFlagsErrorParameter;
#if !OS_CFG_EVENT_FLAGS
    return osFlagsErrorResource;
#else
    osb_bq_lock(ef_id->obj.q);
    ef_id->flags |= flags;
    uint32_t now = ef_id->flags;
    osb_bq_wake_all(ef_id->obj.q);
    osb_bq_unlock(ef_id->obj.q);
    return now;
#endif
}

uint32_t osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags)
{
    if (!ef_valid(ef_id))
        return osFlagsErrorParameter;
    osb_bq_lock(ef_id->obj.q);
    uint32_t prev = ef_id->flags;
    ef_id->flags = prev & ~flags;
    osb_bq_unlock(ef_id->obj.q);
    return prev;
}

uint32_t osEventFlagsGet(osEventFlagsId_t ef_id)
{
    if (!ef_valid(ef_id))
        return 0;
    osb_bq_lock(ef_id->obj.q);
    uint32_t v = ef_id->flags;
    osb_bq_unlock(ef_id->obj.q);
    return v;
}

uint32_t osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags,
                          uint32_t options, uint32_t timeout)
{
    if (!ef_valid(ef_id) || flags == 0 || (options & ~(uint32_t)osFlagsMask))
        return osFlagsErrorParameter;
#if !OS_CFG_EVENT_FLAGS
    return osFlagsErrorResource;
#else
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(ef_id->obj.q);
    for (;;) {
        uint32_t have = ef_id->flags;
        int match = (options & osFlagsWaitAll) ? ((have & flags) == flags)
                                               : ((have & flags) != 0);
        if (match) {
            uint32_t result = (options & osFlagsWaitAll) ? flags : (have & flags);
            if (!(options & osFlagsNoClear))
                ef_id->flags = have & ~flags;
            osb_bq_unlock(ef_id->obj.q);
            return result;
        }
        if (timeout == 0) {
            osb_bq_unlock(ef_id->obj.q);
            return osFlagsErrorResource;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(ef_id->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(ef_id->obj.q);
            return osFlagsErrorTimeout;
        }
    }
#endif
}

osStatus_t osEventFlagsDelete(osEventFlagsId_t ef_id)
{
    if (!ef_valid(ef_id))
        return osErrorParameter;
    osb_bq_destroy(ef_id->obj.q);
    ef_id->obj.magic = 0;
    if (!ef_id->obj.static_cb)
        osb_free(ef_id);
    return osOK;
}
