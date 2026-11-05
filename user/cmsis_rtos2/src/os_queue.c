/*
 * CMSIS-RTOS2 message queues and memory pools.
 *
 * Both are ring/free-list structures protected by the object's blocking
 * queue.  Wakes are broadcast because one blocking queue serves both the
 * "has data / has space" predicates of a queue; a targeted wake could hand
 * the token to the wrong class of waiter.
 */
#include "os_impl.h"


static int mq_valid(osMessageQueueId_t q)
{
    return q && q->obj.magic == OS_MAGIC && q->obj.kind == OS_KIND_MQ;
}

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr)
{
    if (msg_count == 0 || msg_size == 0)
        return NULL;
    if (os_core_init() != 0)
        return NULL;
#if !OS_CFG_MESSAGE_QUEUE
    (void)attr;
    return NULL;
#else
    if (msg_count > SIZE_MAX / msg_size)
        return NULL;

    struct os_messagequeue_cb *q;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*q))
            return NULL;
        q = (struct os_messagequeue_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        q = (struct os_messagequeue_cb *)osb_alloc(sizeof(*q));
        if (!q)
            return NULL;
    }
    os_memzero(q, sizeof(*q));
    q->obj.magic = OS_MAGIC;
    q->obj.kind = OS_KIND_MQ;
    q->obj.static_cb = (uint16_t)static_cb;
    q->obj.q = osb_bq_create();
    q->buf = (uint8_t *)osb_alloc((size_t)msg_count * msg_size);
    q->prio = (uint8_t *)osb_alloc(msg_count);
    if (!q->obj.q || !q->buf || !q->prio) {
        if (q->obj.q)
            osb_bq_destroy(q->obj.q);
        if (q->buf)
            osb_free(q->buf);
        if (q->prio)
            osb_free(q->prio);
        if (!static_cb)
            osb_free(q);
        return NULL;
    }
    os_name_store(q->obj.name, (attr && attr->name) ? attr->name : "mq");
    q->capacity = msg_count;
    q->msg_size = msg_size;
    return q;
#endif
}

const char *osMessageQueueGetName(osMessageQueueId_t mq_id)
{
    return mq_valid(mq_id) ? mq_id->obj.name : NULL;
}

osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
                             uint8_t msg_prio, uint32_t timeout)
{
    if (!mq_valid(mq_id) || !msg_ptr)
        return osErrorParameter;
#if !OS_CFG_MESSAGE_QUEUE
    return osErrorResource;
#else
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(mq_id->obj.q);
    for (;;) {
        if (mq_id->count < mq_id->capacity) {
            /* Higher msg_prio is dequeued first.  Equal priorities keep FIFO
             * order, so insertion goes after every existing equal-priority
             * message.  Queues in the supported profiles are small; the
             * O(n) slot shift is cheaper than a second heap and keeps one
             * contiguous buffer for the trim-friendly MCU build. */
            uint32_t pos = 0;
            while (pos < mq_id->count && mq_id->prio[pos] >= msg_prio)
                pos++;
            for (uint32_t i = mq_id->count; i > pos; i--) {
                os_memcopy(mq_id->buf + (size_t)i * mq_id->msg_size,
                           mq_id->buf + (size_t)(i - 1) * mq_id->msg_size,
                           mq_id->msg_size);
                mq_id->prio[i] = mq_id->prio[i - 1];
            }
            os_memcopy(mq_id->buf + (size_t)pos * mq_id->msg_size,
                       msg_ptr, mq_id->msg_size);
            mq_id->prio[pos] = msg_prio;
            mq_id->count++;
            osb_bq_wake_all(mq_id->obj.q);
            osb_bq_unlock(mq_id->obj.q);
            return osOK;
        }
        if (timeout == 0) {
            osb_bq_unlock(mq_id->obj.q);
            return osErrorResource;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(mq_id->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(mq_id->obj.q);
            return osErrorTimeout;
        }
    }
#endif
}

osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
                             uint8_t *msg_prio, uint32_t timeout)
{
    if (!mq_valid(mq_id) || !msg_ptr)
        return osErrorParameter;
#if !OS_CFG_MESSAGE_QUEUE
    return osErrorResource;
#else
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(mq_id->obj.q);
    for (;;) {
        if (mq_id->count > 0) {
            os_memcopy(msg_ptr, mq_id->buf, mq_id->msg_size);
            if (msg_prio)
                *msg_prio = mq_id->prio[0];
            for (uint32_t i = 0; i + 1 < mq_id->count; i++) {
                os_memcopy(mq_id->buf + (size_t)i * mq_id->msg_size,
                           mq_id->buf + (size_t)(i + 1) * mq_id->msg_size,
                           mq_id->msg_size);
                mq_id->prio[i] = mq_id->prio[i + 1];
            }
            mq_id->count--;
            osb_bq_wake_all(mq_id->obj.q);
            osb_bq_unlock(mq_id->obj.q);
            return osOK;
        }
        if (timeout == 0) {
            osb_bq_unlock(mq_id->obj.q);
            return osErrorResource;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(mq_id->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(mq_id->obj.q);
            return osErrorTimeout;
        }
    }
#endif
}

uint32_t osMessageQueueGetCapacity(osMessageQueueId_t mq_id)
{
    return mq_valid(mq_id) ? mq_id->capacity : 0;
}

uint32_t osMessageQueueGetMsgSize(osMessageQueueId_t mq_id)
{
    return mq_valid(mq_id) ? mq_id->msg_size : 0;
}

uint32_t osMessageQueueGetCount(osMessageQueueId_t mq_id)
{
    if (!mq_valid(mq_id))
        return 0;
    osb_bq_lock(mq_id->obj.q);
    uint32_t n = mq_id->count;
    osb_bq_unlock(mq_id->obj.q);
    return n;
}

uint32_t osMessageQueueGetSpace(osMessageQueueId_t mq_id)
{
    if (!mq_valid(mq_id))
        return 0;
    osb_bq_lock(mq_id->obj.q);
    uint32_t n = mq_id->capacity - mq_id->count;
    osb_bq_unlock(mq_id->obj.q);
    return n;
}

osStatus_t osMessageQueueReset(osMessageQueueId_t mq_id)
{
    if (!mq_valid(mq_id))
        return osErrorParameter;
    osb_bq_lock(mq_id->obj.q);
    mq_id->count = 0;
    osb_bq_wake_all(mq_id->obj.q);
    osb_bq_unlock(mq_id->obj.q);
    return osOK;
}

osStatus_t osMessageQueueDelete(osMessageQueueId_t mq_id)
{
    if (!mq_valid(mq_id))
        return osErrorParameter;
    osb_bq_destroy(mq_id->obj.q);
    osb_free(mq_id->buf);
    osb_free(mq_id->prio);
    mq_id->obj.magic = 0;
    if (!mq_id->obj.static_cb)
        osb_free(mq_id);
    return osOK;
}

/* ------------------------------------------------------------------------- */
/* Memory pool                                                               */
/* ------------------------------------------------------------------------- */

static uint32_t mp_stride(uint32_t block_size)
{
    uint32_t a = (uint32_t)sizeof(void *);
    return (block_size + a - 1U) & ~(a - 1U);
}

static int mp_valid(osMemoryPoolId_t p)
{
    return p && p->obj.magic == OS_MAGIC && p->obj.kind == OS_KIND_MP;
}

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
                                 const osMemoryPoolAttr_t *attr)
{
    if (block_count == 0 || block_size == 0)
        return NULL;
    if (os_core_init() != 0)
        return NULL;
#if !OS_CFG_MEMORY_POOL
    (void)attr;
    return NULL;
#else
    uint32_t stride = mp_stride(block_size);
    if (block_count > SIZE_MAX / stride)
        return NULL;

    struct os_mempool_cb *p;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*p))
            return NULL;
        p = (struct os_mempool_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        p = (struct os_mempool_cb *)osb_alloc(sizeof(*p));
        if (!p)
            return NULL;
    }
    os_memzero(p, sizeof(*p));
    p->obj.magic = OS_MAGIC;
    p->obj.kind = OS_KIND_MP;
    p->obj.static_cb = (uint16_t)static_cb;
    p->obj.q = osb_bq_create();
    p->pool = (uint8_t *)osb_alloc((size_t)block_count * stride);
    p->free_list = (uint32_t *)osb_alloc((size_t)block_count * sizeof(uint32_t));
    if (!p->obj.q || !p->pool || !p->free_list) {
        if (p->obj.q)
            osb_bq_destroy(p->obj.q);
        if (p->pool)
            osb_free(p->pool);
        if (p->free_list)
            osb_free(p->free_list);
        if (!static_cb)
            osb_free(p);
        return NULL;
    }
    os_name_store(p->obj.name, (attr && attr->name) ? attr->name : "mp");
    p->capacity = block_count;
    p->block_size = stride;
    p->used = 0;
    p->free_head = 0;
    for (uint32_t i = 0; i < block_count; i++)
        p->free_list[i] = (i + 1 < block_count) ? (i + 1) : UINT32_MAX;
    return p;
#endif
}

const char *osMemoryPoolGetName(osMemoryPoolId_t mp_id)
{
    return mp_valid(mp_id) ? mp_id->obj.name : NULL;
}

void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout)
{
    if (!mp_valid(mp_id))
        return NULL;
#if !OS_CFG_MEMORY_POOL
    return NULL;
#else
    osThreadId_t self = os_current_thread();
    uint64_t deadline = os_deadline_ns(timeout);

    osb_bq_lock(mp_id->obj.q);
    for (;;) {
        if (mp_id->free_head != UINT32_MAX) {
            uint32_t idx = mp_id->free_head;
            mp_id->free_head = mp_id->free_list[idx];
            mp_id->used++;
            void *block = mp_id->pool + (size_t)idx * mp_id->block_size;
            osb_bq_unlock(mp_id->obj.q);
            return block;
        }
        if (timeout == 0) {
            osb_bq_unlock(mp_id->obj.q);
            return NULL;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(mp_id->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(mp_id->obj.q);
            return NULL;
        }
    }
#endif
}

osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block)
{
    if (!mp_valid(mp_id) || !block)
        return osErrorParameter;
#if !OS_CFG_MEMORY_POOL
    return osErrorResource;
#else
    uintptr_t base = (uintptr_t)mp_id->pool;
    uintptr_t addr = (uintptr_t)block;
    if (addr < base || addr >= base + (uintptr_t)mp_id->capacity * mp_id->block_size)
        return osErrorParameter;
    uintptr_t off = addr - base;
    if (off % mp_id->block_size)
        return osErrorParameter;
    uint32_t idx = (uint32_t)(off / mp_id->block_size);

    osb_bq_lock(mp_id->obj.q);
    /* Reject a double free: walking the list is O(n), but a pool is small and
     * this is the only way to keep a corrupt free list from being fatal. */
    for (uint32_t i = mp_id->free_head; i != UINT32_MAX; i = mp_id->free_list[i]) {
        if (i == idx) {
            osb_bq_unlock(mp_id->obj.q);
            return osErrorParameter;
        }
    }
    mp_id->free_list[idx] = mp_id->free_head;
    mp_id->free_head = idx;
    mp_id->used--;
    osb_bq_wake_all(mp_id->obj.q);
    osb_bq_unlock(mp_id->obj.q);
    return osOK;
#endif
}

uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id)
{
    return mp_valid(mp_id) ? mp_id->capacity : 0;
}

uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id)
{
    return mp_valid(mp_id) ? mp_id->block_size : 0;
}

uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id)
{
    if (!mp_valid(mp_id))
        return 0;
    osb_bq_lock(mp_id->obj.q);
    uint32_t n = mp_id->used;
    osb_bq_unlock(mp_id->obj.q);
    return n;
}

uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id)
{
    if (!mp_valid(mp_id))
        return 0;
    osb_bq_lock(mp_id->obj.q);
    uint32_t n = mp_id->capacity - mp_id->used;
    osb_bq_unlock(mp_id->obj.q);
    return n;
}

osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id)
{
    if (!mp_valid(mp_id))
        return osErrorParameter;
    osb_bq_destroy(mp_id->obj.q);
    osb_free(mp_id->pool);
    osb_free(mp_id->free_list);
    mp_id->obj.magic = 0;
    if (!mp_id->obj.static_cb)
        osb_free(mp_id);
    return osOK;
}
