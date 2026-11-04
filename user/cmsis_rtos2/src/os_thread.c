/*
 * CMSIS-RTOS2 threads and thread flags.
 *
 * Thread creation publishes a small control block and hands the actual
 * scheduling to the backend.  The entry trampoline lives in os_kernel.c so
 * every backend shares the same state transitions.
 */
#include "os_impl.h"


static int thread_valid(osThreadId_t t)
{
    return t && t->obj.magic == OS_MAGIC && t->obj.kind == OS_KIND_THREAD;
}

void os_thread_release(osThreadId_t t)
{
    if (!t)
        return;
    os_thread_registry_remove(t);
    if (t->owned_stack) {
        osb_free(t->owned_stack);
        t->owned_stack = NULL;
    }
    if (t->obj.q) {
        osb_bq_destroy(t->obj.q);
        t->obj.q = NULL;
    }
    t->obj.magic = 0;
    if (!t->obj.static_cb)
        osb_free(t);
}

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
                         const osThreadAttr_t *attr)
{
    if (!func)
        return NULL;
    if (os_core_init() != 0)
        return NULL;

    struct os_thread_cb *t = NULL;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*t))
            return NULL;
        t = (struct os_thread_cb *)attr->cb_mem;
        os_memzero(t, sizeof(*t));
        static_cb = 1;
    } else {
        t = (struct os_thread_cb *)osb_alloc(sizeof(*t));
        if (!t)
            return NULL;
        os_memzero(t, sizeof(*t));
    }

    t->obj.magic = OS_MAGIC;
    t->obj.kind = OS_KIND_THREAD;
    t->obj.static_cb = (uint16_t)static_cb;
    t->obj.q = osb_bq_create();
    if (!t->obj.q) {
        if (!static_cb)
            osb_free(t);
        return NULL;
    }

    os_name_store(t->obj.name, (attr && attr->name) ? attr->name : "thread");
    t->func = func;
    t->arg = argument;
    t->priority = (attr && attr->priority != osPriorityNone)
                      ? attr->priority : osPriorityNormal;
    /* osPriorityNone and osPriorityISR are not thread priorities. */
    if (t->priority < osPriorityIdle || t->priority >= osPriorityISR) {
        osb_bq_destroy(t->obj.q);
        if (!static_cb)
            osb_free(t);
        return NULL;
    }
    t->attr_bits = attr ? attr->attr_bits : 0U;
    t->stack_size = (attr && attr->stack_size)
                        ? attr->stack_size : OS_CFG_DEFAULT_STACK_SIZE;
    t->stack_mem = attr ? attr->stack_mem : NULL;
    t->static_stack = (t->stack_mem != NULL);
    t->state = osThreadReady;

    if (os_thread_registry_add(t) != 0) {
        osb_bq_destroy(t->obj.q);
        if (!static_cb)
            osb_free(t);
        return NULL;
    }

    if (osb_thread_start(t) != 0) {
        os_thread_release(t);
        return NULL;
    }

    if (t->attr_bits & osThreadDetached) {
        if (osb_thread_detach(t) != 0) {
            /* The thread is already running; leaving it joinable only leaks
             * the control block, which the trampoline will free on exit. */
        }
        t->detached = 1;
    }

    return t;
}

const char *osThreadGetName(osThreadId_t thread_id)
{
    return thread_valid(thread_id) ? thread_id->obj.name : NULL;
}

osThreadId_t osThreadGetId(void)
{
    return os_current_thread();
}

osThreadState_t osThreadGetState(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osThreadError;
    return (osThreadState_t)thread_id->state;
}

osStatus_t osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    if (priority <= osPriorityNone || priority >= osPriorityISR)
        return osErrorParameter;
    /*
     * A task that is currently executing a priority donation reports the
     * donated (higher) priority.  Changing its base underneath the kernel's
     * saved snapshot would make the later restore resurrect the old base, so
     * reject it instead of silently applying a change that cannot survive.
     */
    osPriority_t effective = osb_thread_get_priority(thread_id);
    if (effective != osPriorityNone && effective != osPriorityError &&
        effective > thread_id->priority)
        return osError;
    thread_id->priority = priority;
    if (osb_thread_set_priority(thread_id, priority) != 0)
        return osError;
    return osOK;
}

osPriority_t osThreadGetPriority(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osPriorityError;
    osPriority_t p = osb_thread_get_priority(thread_id);
    return p == osPriorityNone ? thread_id->priority : p;
}

osStatus_t osThreadYield(void)
{
    osb_thread_yield();
    return osOK;
}

osStatus_t osThreadSuspend(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    thread_id->suspend_req = 1;
    if (osb_thread_suspend(thread_id) != 0)
        return osError;
    return osOK;
}

osStatus_t osThreadResume(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    thread_id->suspend_req = 0;
    if (osb_thread_resume(thread_id) != 0)
        return osError;
    return osOK;
}

osStatus_t osThreadDetach(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    if (thread_id->detached)
        return osOK;
    if (osb_thread_detach(thread_id) != 0)
        return osError;
    thread_id->detached = 1;
    return osOK;
}

osStatus_t osThreadJoin(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    if (thread_id->detached)
        return osErrorResource;
    if (thread_id == os_current_thread())
        return osErrorResource;
    if (osb_thread_join(thread_id) != 0)
        return osError;
    os_thread_release(thread_id);
    return osOK;
}

void osThreadExit(void)
{
    osThreadId_t self = os_current_thread();
    if (thread_valid(self)) {
        self->state = osThreadTerminated;
        if (self->detached)
            os_thread_release(self);
    }
    osb_thread_exit();
}

osStatus_t osThreadTerminate(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return osErrorParameter;
    if (thread_id == os_current_thread()) {
        osThreadExit();
        return osOK;
    }
    if (osb_thread_terminate(thread_id) != 0)
        return osError;
    return osOK;
}

uint32_t osThreadGetStackSize(osThreadId_t thread_id)
{
    return thread_valid(thread_id) ? thread_id->stack_size : 0;
}

uint32_t osThreadGetStackSpace(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return 0;
    return osb_thread_stack_space(thread_id);
}

uint32_t osThreadGetCount(void)
{
    return os_thread_registry_count();
}

uint32_t osThreadEnumerate(osThreadId_t *thread_array, uint32_t array_items)
{
    if (!thread_array || !array_items)
        return 0;
    return os_thread_registry_enum(thread_array, array_items);
}

osStatus_t osThreadSetAffinity(osThreadId_t thread_id, uint32_t cpu_set)
{
    if (!thread_valid(thread_id) || cpu_set == 0)
        return osErrorParameter;
    return osb_thread_set_affinity(thread_id, cpu_set) == 0 ? osOK : osError;
}

uint32_t osThreadGetAffinity(osThreadId_t thread_id)
{
    if (!thread_valid(thread_id))
        return 0;
    return osb_thread_get_affinity(thread_id);
}

/* ------------------------------------------------------------------------- */
/* Thread flags                                                              */
/* ------------------------------------------------------------------------- */

static int flags_match(uint32_t have, uint32_t want, uint32_t options)
{
    if (options & osFlagsWaitAll)
        return (have & want) == want;
    return (have & want) != 0;
}

uint32_t osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags)
{
    if (!thread_valid(thread_id))
        return osFlagsErrorParameter;
    osb_bq_lock(thread_id->obj.q);
    uint32_t now = thread_id->flags | flags;
    thread_id->flags = now;
    osb_bq_wake_all(thread_id->obj.q);
    osb_bq_unlock(thread_id->obj.q);
    return now;
}

uint32_t osThreadFlagsClear(uint32_t flags)
{
    osThreadId_t self = os_current_thread();
    if (!thread_valid(self))
        return osFlagsErrorUnknown;
    osb_bq_lock(self->obj.q);
    uint32_t prev = self->flags;
    self->flags &= ~flags;
    osb_bq_unlock(self->obj.q);
    return prev;
}

uint32_t osThreadFlagsGet(void)
{
    osThreadId_t self = os_current_thread();
    if (!thread_valid(self))
        return 0;
    osb_bq_lock(self->obj.q);
    uint32_t v = self->flags;
    osb_bq_unlock(self->obj.q);
    return v;
}

uint32_t osThreadFlagsWait(uint32_t flags, uint32_t options, uint32_t timeout)
{
    osThreadId_t self = os_current_thread();
    if (!thread_valid(self) || flags == 0)
        return osFlagsErrorParameter;
    if (options & ~(uint32_t)osFlagsMask)
        return osFlagsErrorParameter;

    uint64_t deadline = os_deadline_ns(timeout);
    osb_bq_lock(self->obj.q);
    for (;;) {
        uint32_t have = self->flags;
        if (flags_match(have, flags, options)) {
            uint32_t result = (options & osFlagsWaitAll) ? flags : (have & flags);
            if (!(options & osFlagsNoClear))
                self->flags = have & ~flags;
            osb_bq_unlock(self->obj.q);
            return result;
        }
        if (timeout == 0) {
            osb_bq_unlock(self->obj.q);
            return osFlagsErrorResource;
        }
        self->state = osThreadBlocked;
        int rc = osb_bq_wait_until(self->obj.q, deadline);
        self->state = osThreadRunning;
        if (rc == -OS_ETIMEDOUT) {
            osb_bq_unlock(self->obj.q);
            return osFlagsErrorTimeout;
        }
    }
}
